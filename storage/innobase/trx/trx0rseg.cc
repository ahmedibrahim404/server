/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0rseg.cc
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rseg.h"
#include "trx0undo.h"
#include "fut0lst.h"
#include "srv0srv.h"
#include "trx0purge.h"
#include "srv0mon.h"
#include "fsp0sysspace.h"

#include <algorithm>

/** Creates a rollback segment header.
This function is called only when a new rollback segment is created in
the database.
@param[in]	space		space id
@param[in]	max_size	max size in pages
@param[in]	rseg_id		rollback segment identifier
@param[in,out]	sys_header	the TRX_SYS page (NULL for temporary rseg)
@param[in,out]	mtr		mini-transaction
@return page number of the created segment, FIL_NULL if fail */
ulint
trx_rseg_header_create(
	ulint			space,
	ulint			max_size,
	ulint			rseg_id,
	buf_block_t*		sys_header,
	mtr_t*			mtr)
{
	ulint		page_no;
	trx_rsegf_t*	rsegf;
	buf_block_t*	block;

	ut_ad(mtr);
	ut_ad(mtr_memo_contains(mtr, fil_space_get_latch(space, NULL),
				MTR_MEMO_X_LOCK));
	ut_ad(!sys_header == (space == SRV_TMP_SPACE_ID));

	/* Allocate a new file segment for the rollback segment */
	block = fseg_create(space, 0, TRX_RSEG + TRX_RSEG_FSEG_HEADER, mtr);

	if (block == NULL) {
		/* No space left */

		return(FIL_NULL);
	}

	buf_block_dbg_add_level(block, SYNC_RSEG_HEADER_NEW);

	page_no = block->page.id.page_no();

	/* Get the rollback segment file page */
	rsegf = trx_rsegf_get_new(space, page_no, mtr);

	/* Initialize max size field */
	mlog_write_ulint(rsegf + TRX_RSEG_MAX_SIZE, max_size,
			 MLOG_4BYTES, mtr);

	/* Initialize the history list */

	mlog_write_ulint(rsegf + TRX_RSEG_HISTORY_SIZE, 0, MLOG_4BYTES, mtr);
	flst_init(rsegf + TRX_RSEG_HISTORY, mtr);

	/* Reset the undo log slots */
	for (ulint i = 0; i < TRX_RSEG_N_SLOTS; i++) {

		trx_rsegf_set_nth_undo(rsegf, i, FIL_NULL, mtr);
	}

	if (sys_header) {
		/* Add the rollback segment info to the free slot in
		the trx system header */

		mlog_write_ulint(TRX_SYS + TRX_SYS_RSEGS
				 + TRX_SYS_RSEG_SPACE
				 + rseg_id * TRX_SYS_RSEG_SLOT_SIZE
				 + sys_header->frame,
				 space, MLOG_4BYTES, mtr);
		mlog_write_ulint(TRX_SYS + TRX_SYS_RSEGS
				 + TRX_SYS_RSEG_PAGE_NO
				 + rseg_id * TRX_SYS_RSEG_SLOT_SIZE
				 + sys_header->frame,
				 page_no, MLOG_4BYTES, mtr);
	}

	return(page_no);
}

/** Free a rollback segment in memory. */
void
trx_rseg_mem_free(trx_rseg_t* rseg)
{
	trx_undo_t*	undo;
	trx_undo_t*	next_undo;

	mutex_free(&rseg->mutex);

	/* There can't be any active transactions. */
	ut_a(UT_LIST_GET_LEN(rseg->undo_list) == 0);
	ut_a(UT_LIST_GET_LEN(rseg->old_insert_list) == 0);

	for (undo = UT_LIST_GET_FIRST(rseg->undo_cached);
	     undo != NULL;
	     undo = next_undo) {

		next_undo = UT_LIST_GET_NEXT(undo_list, undo);

		UT_LIST_REMOVE(rseg->undo_cached, undo);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);

		trx_undo_mem_free(undo);
	}

	ut_free(rseg);
}

/** Create a rollback segment object.
@param[in]	id		rollback segment id
@param[in]	space		space where the segment is placed
@param[in]	page_no		page number of the segment header */
static
trx_rseg_t*
trx_rseg_mem_create(ulint id, ulint space, ulint page_no)
{
	trx_rseg_t* rseg = static_cast<trx_rseg_t*>(
		ut_zalloc_nokey(sizeof *rseg));

	rseg->id = id;
	rseg->space = space;
	rseg->page_no = page_no;
	rseg->last_page_no = FIL_NULL;

	mutex_create(rseg->is_persistent()
		     ? LATCH_ID_REDO_RSEG : LATCH_ID_NOREDO_RSEG,
		     &rseg->mutex);

	UT_LIST_INIT(rseg->undo_list, &trx_undo_t::undo_list);
	UT_LIST_INIT(rseg->old_insert_list, &trx_undo_t::undo_list);
	UT_LIST_INIT(rseg->undo_cached, &trx_undo_t::undo_list);

	return(rseg);
}

/** Restore the state of a persistent rollback segment.
@param[in,out]	rseg	persistent rollback segment
@param[in,out]	mtr	mini-transaction */
static
void
trx_rseg_mem_restore(trx_rseg_t* rseg, mtr_t* mtr)
{
	ulint		len;
	fil_addr_t	node_addr;
	trx_rsegf_t*	rseg_header;
	trx_ulogf_t*	undo_log_hdr;
	ulint		sum_of_undo_sizes;

	rseg_header = trx_rsegf_get_new(rseg->space, rseg->page_no, mtr);

	rseg->max_size = mtr_read_ulint(
		rseg_header + TRX_RSEG_MAX_SIZE, MLOG_4BYTES, mtr);

	/* Initialize the undo log lists according to the rseg header */

	sum_of_undo_sizes = trx_undo_lists_init(rseg);

	rseg->curr_size = mtr_read_ulint(
		rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr)
		+ 1 + sum_of_undo_sizes;

	len = flst_get_len(rseg_header + TRX_RSEG_HISTORY);

	if (len > 0) {
		my_atomic_addlint(&trx_sys.rseg_history_len, len);

		node_addr = trx_purge_get_log_from_hist(
			flst_get_last(rseg_header + TRX_RSEG_HISTORY, mtr));

		rseg->last_page_no = node_addr.page;
		rseg->last_offset = node_addr.boffset;

		undo_log_hdr = trx_undo_page_get(
			page_id_t(rseg->space, node_addr.page), mtr)
			+ node_addr.boffset;

		rseg->last_trx_no = mach_read_from_8(
			undo_log_hdr + TRX_UNDO_TRX_NO);
		unsigned purge = mach_read_from_2(
			undo_log_hdr + TRX_UNDO_NEEDS_PURGE);
		ut_ad(purge <= 1);
		rseg->needs_purge = purge != 0;

		TrxUndoRsegs elem(rseg->last_trx_no);
		elem.push_back(rseg);

		if (rseg->last_page_no != FIL_NULL) {

			/* There is no need to cover this operation by the purge
			mutex because we are still bootstrapping. */

			purge_sys->purge_queue.push(elem);
		}
	}
}

/** Initialize the rollback segments in memory at database startup. */
void
trx_rseg_array_init()
{
	mtr_t	mtr;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		mtr.start();
		if (const buf_block_t* sys = trx_sysf_get(&mtr, false)) {
			const uint32_t	page_no = trx_sysf_rseg_get_page_no(
				sys, rseg_id);
			if (page_no != FIL_NULL) {
				trx_rseg_t* rseg = trx_rseg_mem_create(
					rseg_id, trx_sysf_rseg_get_space(
						sys, rseg_id),
					page_no);
				ut_ad(rseg->is_persistent());
				ut_ad(rseg->id == rseg_id);
				ut_ad(!trx_sys.rseg_array[rseg_id]);
				trx_sys.rseg_array[rseg_id] = rseg;
				trx_rseg_mem_restore(rseg, &mtr);
			}
		}

		mtr.commit();
	}
}

/** Create a persistent rollback segment.
@param[in]	space_id	system or undo tablespace id
@return pointer to new rollback segment
@retval	NULL	on failure */
trx_rseg_t*
trx_rseg_create(ulint space_id)
{
	trx_rseg_t*		rseg = NULL;
	mtr_t			mtr;

	mtr.start();

	/* To obey the latching order, acquire the file space
	x-latch before the trx_sys.mutex. */
#ifdef UNIV_DEBUG
	const fil_space_t*	space =
#endif /* UNIV_DEBUG */
		mtr_x_lock_space(space_id, &mtr);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);

	if (buf_block_t* sys_header = trx_sysf_get(&mtr)) {
		ulint	rseg_id = trx_sys_rseg_find_free(sys_header);
		ulint	page_no = rseg_id == ULINT_UNDEFINED
			? FIL_NULL
			: trx_rseg_header_create(space_id, ULINT_MAX,
						 rseg_id, sys_header, &mtr);
		if (page_no != FIL_NULL) {
			ut_ad(trx_sysf_rseg_get_space(sys_header, rseg_id)
			      == space_id);
			rseg = trx_rseg_mem_create(rseg_id, space_id, page_no);
			ut_ad(rseg->id == rseg_id);
			ut_ad(rseg->is_persistent());
			ut_ad(!trx_sys.rseg_array[rseg->id]);
			trx_sys.rseg_array[rseg->id] = rseg;
			trx_rseg_mem_restore(rseg, &mtr);
		}
	}

	mtr.commit();

	return(rseg);
}

/** Create the temporary rollback segments. */
void
trx_temp_rseg_create()
{
	mtr_t		mtr;

	for (ulong i = 0; i < TRX_SYS_N_RSEGS; i++) {
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
#ifdef UNIV_DEBUG
		const fil_space_t*	space =
#endif /* UNIV_DEBUG */
			mtr_x_lock_space(SRV_TMP_SPACE_ID, &mtr);
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY);

		ulint page_no = trx_rseg_header_create(
			SRV_TMP_SPACE_ID, ULINT_MAX, i, NULL, &mtr);
		trx_rseg_t* rseg = trx_rseg_mem_create(
			i, SRV_TMP_SPACE_ID, page_no);
		ut_ad(!rseg->is_persistent());
		ut_ad(!trx_sys.temp_rsegs[i]);
		trx_sys.temp_rsegs[i] = rseg;
		trx_rseg_mem_restore(rseg, &mtr);
		mtr.commit();
	}
}

/********************************************************************
Get the number of unique rollback tablespaces in use except space id 0.
The last space id will be the sentinel value ULINT_UNDEFINED. The array
will be sorted on space id. Note: space_ids should have have space for
TRX_SYS_N_RSEGS + 1 elements.
@return number of unique rollback tablespaces in use. */
ulint
trx_rseg_get_n_undo_tablespaces(
/*============================*/
	ulint*		space_ids)	/*!< out: array of space ids of
					UNDO tablespaces */
{
	mtr_t mtr;
	mtr.start();

	buf_block_t* sys_header = trx_sysf_get(&mtr, false);
	if (!sys_header) {
		mtr.commit();
		return 0;
	}

	ulint* end = space_ids;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		uint32_t page_no = trx_sysf_rseg_get_page_no(sys_header,
							     rseg_id);

		if (page_no == FIL_NULL) {
			continue;
		}

		if (ulint space = trx_sysf_rseg_get_space(sys_header,
							  rseg_id)) {
			if (std::find(space_ids, end, space) == end) {
				*end++ = space;
			}
		}
	}

	mtr.commit();

	ut_a(end - space_ids <= TRX_SYS_N_RSEGS);
	*end = ULINT_UNDEFINED;

	std::sort(space_ids, end);

	return ulint(end - space_ids);
}
