// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_btree.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_alloc.h"
#include "xfs_log.h"
#include "xfs_btree_staging.h"
#include "xfs_ag.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_health.h"
#include "xfs_buf_mem.h"
#include "xfs_btree_mem.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_bmap.h"
#include "xfs_rmap.h"
#include "xfs_quota.h"
#include "xfs_metafile.h"
#include "xfs_rtrefcount_btree.h"

/*
 * Btree magic numbers.
 */
uint32_t
xfs_btree_magic(
	struct xfs_mount		*mp,
	const struct xfs_btree_ops	*ops)
{
	int				idx = xfs_has_crc(mp) ? 1 : 0;
	__be32				magic = ops->buf_ops->magic[idx];

	/* Ensure we asked for crc for crc-only magics. */
	ASSERT(magic != 0);
	return be32_to_cpu(magic);
}

/*
 * These sibling pointer checks are optimised for null sibling pointers. This
 * happens a lot, and we don't need to byte swap at runtime if the sibling
 * pointer is NULL.
 *
 * These are explicitly marked at inline because the cost of calling them as
 * functions instead of inlining them is about 36 bytes extra code per call site
 * on x86-64. Yes, gcc-11 fails to inline them, and explicit inlining of these
 * two sibling check functions reduces the compiled code size by over 300
 * bytes.
 */
static inline xfs_failaddr_t
xfs_btree_check_fsblock_siblings(
	struct xfs_mount	*mp,
	xfs_fsblock_t		fsb,
	__be64			dsibling)
{
	xfs_fsblock_t		sibling;

	if (dsibling == cpu_to_be64(NULLFSBLOCK))
		return NULL;

	sibling = be64_to_cpu(dsibling);
	if (sibling == fsb)
		return __this_address;
	if (!xfs_verify_fsbno(mp, sibling))
		return __this_address;
	return NULL;
}

static inline xfs_failaddr_t
xfs_btree_check_memblock_siblings(
	struct xfs_buftarg	*btp,
	xfbno_t			bno,
	__be64			dsibling)
{
	xfbno_t			sibling;

	if (dsibling == cpu_to_be64(NULLFSBLOCK))
		return NULL;

	sibling = be64_to_cpu(dsibling);
	if (sibling == bno)
		return __this_address;
	if (!xmbuf_verify_daddr(btp, xfbno_to_daddr(sibling)))
		return __this_address;
	return NULL;
}

static inline xfs_failaddr_t
xfs_btree_check_agblock_siblings(
	struct xfs_perag	*pag,
	xfs_agblock_t		agbno,
	__be32			dsibling)
{
	xfs_agblock_t		sibling;

	if (dsibling == cpu_to_be32(NULLAGBLOCK))
		return NULL;

	sibling = be32_to_cpu(dsibling);
	if (sibling == agbno)
		return __this_address;
	if (!xfs_verify_agbno(pag, sibling))
		return __this_address;
	return NULL;
}

static xfs_failaddr_t
__xfs_btree_check_lblock_hdr(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = cur->bc_mp;

	if (xfs_has_crc(mp)) {
		if (!uuid_equal(&block->bb_u.l.bb_uuid, &mp->m_sb.sb_meta_uuid))
			return __this_address;
		if (block->bb_u.l.bb_blkno !=
		    cpu_to_be64(bp ? xfs_buf_daddr(bp) : XFS_BUF_DADDR_NULL))
			return __this_address;
		if (block->bb_u.l.bb_pad != cpu_to_be32(0))
			return __this_address;
	}

	if (be32_to_cpu(block->bb_magic) != xfs_btree_magic(mp, cur->bc_ops))
		return __this_address;
	if (be16_to_cpu(block->bb_level) != level)
		return __this_address;
	if (be16_to_cpu(block->bb_numrecs) >
	    cur->bc_ops->get_maxrecs(cur, level))
		return __this_address;

	return NULL;
}

/*
 * Check a long btree block header.  Return the address of the failing check,
 * or NULL if everything is ok.
 */
static xfs_failaddr_t
__xfs_btree_check_fsblock(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_failaddr_t		fa;
	xfs_fsblock_t		fsb;

	fa = __xfs_btree_check_lblock_hdr(cur, block, level, bp);
	if (fa)
		return fa;

	/*
	 * For inode-rooted btrees, the root block sits in the inode fork.  In
	 * that case bp is NULL, and the block must not have any siblings.
	 */
	if (!bp) {
		if (block->bb_u.l.bb_leftsib != cpu_to_be64(NULLFSBLOCK))
			return __this_address;
		if (block->bb_u.l.bb_rightsib != cpu_to_be64(NULLFSBLOCK))
			return __this_address;
		return NULL;
	}

	fsb = XFS_DADDR_TO_FSB(mp, xfs_buf_daddr(bp));
	fa = xfs_btree_check_fsblock_siblings(mp, fsb,
			block->bb_u.l.bb_leftsib);
	if (!fa)
		fa = xfs_btree_check_fsblock_siblings(mp, fsb,
				block->bb_u.l.bb_rightsib);
	return fa;
}

/*
 * Check an in-memory btree block header.  Return the address of the failing
 * check, or NULL if everything is ok.
 */
static xfs_failaddr_t
__xfs_btree_check_memblock(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*bp)
{
	struct xfs_buftarg	*btp = cur->bc_mem.xfbtree->target;
	xfs_failaddr_t		fa;
	xfbno_t			bno;

	fa = __xfs_btree_check_lblock_hdr(cur, block, level, bp);
	if (fa)
		return fa;

	bno = xfs_daddr_to_xfbno(xfs_buf_daddr(bp));
	fa = xfs_btree_check_memblock_siblings(btp, bno,
			block->bb_u.l.bb_leftsib);
	if (!fa)
		fa = xfs_btree_check_memblock_siblings(btp, bno,
				block->bb_u.l.bb_rightsib);
	return fa;
}

/*
 * Check a short btree block header.  Return the address of the failing check,
 * or NULL if everything is ok.
 */
static xfs_failaddr_t
__xfs_btree_check_agblock(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_perag	*pag = to_perag(cur->bc_group);
	xfs_failaddr_t		fa;
	xfs_agblock_t		agbno;

	if (xfs_has_crc(mp)) {
		if (!uuid_equal(&block->bb_u.s.bb_uuid, &mp->m_sb.sb_meta_uuid))
			return __this_address;
		if (block->bb_u.s.bb_blkno != cpu_to_be64(xfs_buf_daddr(bp)))
			return __this_address;
	}

	if (be32_to_cpu(block->bb_magic) != xfs_btree_magic(mp, cur->bc_ops))
		return __this_address;
	if (be16_to_cpu(block->bb_level) != level)
		return __this_address;
	if (be16_to_cpu(block->bb_numrecs) >
	    cur->bc_ops->get_maxrecs(cur, level))
		return __this_address;

	agbno = xfs_daddr_to_agbno(mp, xfs_buf_daddr(bp));
	fa = xfs_btree_check_agblock_siblings(pag, agbno,
			block->bb_u.s.bb_leftsib);
	if (!fa)
		fa = xfs_btree_check_agblock_siblings(pag, agbno,
				block->bb_u.s.bb_rightsib);
	return fa;
}

/*
 * Internal btree block check.
 *
 * Return NULL if the block is ok or the address of the failed check otherwise.
 */
xfs_failaddr_t
__xfs_btree_check_block(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*bp)
{
	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_MEM:
		return __xfs_btree_check_memblock(cur, block, level, bp);
	case XFS_BTREE_TYPE_AG:
		return __xfs_btree_check_agblock(cur, block, level, bp);
	case XFS_BTREE_TYPE_INODE:
		return __xfs_btree_check_fsblock(cur, block, level, bp);
	default:
		ASSERT(0);
		return __this_address;
	}
}

static inline unsigned int xfs_btree_block_errtag(struct xfs_btree_cur *cur)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_SHORT_PTR_LEN)
		return XFS_ERRTAG_BTREE_CHECK_SBLOCK;
	return XFS_ERRTAG_BTREE_CHECK_LBLOCK;
}

/*
 * Debug routine: check that block header is ok.
 */
int
xfs_btree_check_block(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	struct xfs_btree_block	*block,	/* generic btree block pointer */
	int			level,	/* level of the btree block */
	struct xfs_buf		*bp)	/* buffer containing block, if any */
{
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_failaddr_t		fa;

	fa = __xfs_btree_check_block(cur, block, level, bp);
	if (XFS_IS_CORRUPT(mp, fa != NULL) ||
	    XFS_TEST_ERROR(false, mp, xfs_btree_block_errtag(cur))) {
		if (bp)
			trace_xfs_btree_corrupt(bp, _RET_IP_);
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}
	return 0;
}

int
__xfs_btree_check_ptr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				index,
	int				level)
{
	if (level <= 0)
		return -EFSCORRUPTED;

	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_MEM:
		if (!xfbtree_verify_bno(cur->bc_mem.xfbtree,
				be64_to_cpu((&ptr->l)[index])))
			return -EFSCORRUPTED;
		break;
	case XFS_BTREE_TYPE_INODE:
		if (!xfs_verify_fsbno(cur->bc_mp,
				be64_to_cpu((&ptr->l)[index])))
			return -EFSCORRUPTED;
		break;
	case XFS_BTREE_TYPE_AG:
		if (!xfs_verify_agbno(to_perag(cur->bc_group),
				be32_to_cpu((&ptr->s)[index])))
			return -EFSCORRUPTED;
		break;
	}

	return 0;
}

/*
 * Check that a given (indexed) btree pointer at a certain level of a
 * btree is valid and doesn't point past where it should.
 */
static int
xfs_btree_check_ptr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				index,
	int				level)
{
	int				error;

	error = __xfs_btree_check_ptr(cur, ptr, index, level);
	if (error) {
		switch (cur->bc_ops->type) {
		case XFS_BTREE_TYPE_MEM:
			xfs_err(cur->bc_mp,
"In-memory: Corrupt %sbt flags 0x%x pointer at level %d index %d fa %pS.",
				cur->bc_ops->name, cur->bc_flags, level, index,
				__this_address);
			break;
		case XFS_BTREE_TYPE_INODE:
			xfs_err(cur->bc_mp,
"Inode %llu fork %d: Corrupt %sbt pointer at level %d index %d.",
				cur->bc_ino.ip->i_ino,
				cur->bc_ino.whichfork, cur->bc_ops->name,
				level, index);
			break;
		case XFS_BTREE_TYPE_AG:
			xfs_err(cur->bc_mp,
"AG %u: Corrupt %sbt pointer at level %d index %d.",
				cur->bc_group->xg_gno, cur->bc_ops->name,
				level, index);
			break;
		}
		xfs_btree_mark_sick(cur);
	}

	return error;
}

#ifdef DEBUG
# define xfs_btree_debug_check_ptr	xfs_btree_check_ptr
#else
# define xfs_btree_debug_check_ptr(...)	(0)
#endif

/*
 * Calculate CRC on the whole btree block and stuff it into the
 * long-form btree header.
 *
 * Prior to calculting the CRC, pull the LSN out of the buffer log item and put
 * it into the buffer so recovery knows what the last modification was that made
 * it to disk.
 */
void
xfs_btree_fsblock_calc_crc(
	struct xfs_buf		*bp)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buf_log_item	*bip = bp->b_log_item;

	if (!xfs_has_crc(bp->b_mount))
		return;
	if (bip)
		block->bb_u.l.bb_lsn = cpu_to_be64(bip->bli_item.li_lsn);
	xfs_buf_update_cksum(bp, XFS_BTREE_LBLOCK_CRC_OFF);
}

bool
xfs_btree_fsblock_verify_crc(
	struct xfs_buf		*bp)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_mount	*mp = bp->b_mount;

	if (xfs_has_crc(mp)) {
		if (!xfs_log_check_lsn(mp, be64_to_cpu(block->bb_u.l.bb_lsn)))
			return false;
		return xfs_buf_verify_cksum(bp, XFS_BTREE_LBLOCK_CRC_OFF);
	}

	return true;
}

/*
 * Calculate CRC on the whole btree block and stuff it into the
 * short-form btree header.
 *
 * Prior to calculting the CRC, pull the LSN out of the buffer log item and put
 * it into the buffer so recovery knows what the last modification was that made
 * it to disk.
 */
void
xfs_btree_agblock_calc_crc(
	struct xfs_buf		*bp)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buf_log_item	*bip = bp->b_log_item;

	if (!xfs_has_crc(bp->b_mount))
		return;
	if (bip)
		block->bb_u.s.bb_lsn = cpu_to_be64(bip->bli_item.li_lsn);
	xfs_buf_update_cksum(bp, XFS_BTREE_SBLOCK_CRC_OFF);
}

bool
xfs_btree_agblock_verify_crc(
	struct xfs_buf		*bp)
{
	struct xfs_btree_block  *block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_mount	*mp = bp->b_mount;

	if (xfs_has_crc(mp)) {
		if (!xfs_log_check_lsn(mp, be64_to_cpu(block->bb_u.s.bb_lsn)))
			return false;
		return xfs_buf_verify_cksum(bp, XFS_BTREE_SBLOCK_CRC_OFF);
	}

	return true;
}

static int
xfs_btree_free_block(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	int			error;

	trace_xfs_btree_free_block(cur, bp);

	/*
	 * Don't allow block freeing for a staging cursor, because staging
	 * cursors do not support regular btree modifications.
	 */
	if (unlikely(cur->bc_flags & XFS_BTREE_STAGING)) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	error = cur->bc_ops->free_block(cur, bp);
	if (!error) {
		xfs_trans_binval(cur->bc_tp, bp);
		XFS_BTREE_STATS_INC(cur, free);
	}
	return error;
}

/*
 * Delete the btree cursor.
 */
void
xfs_btree_del_cursor(
	struct xfs_btree_cur	*cur,		/* btree cursor */
	int			error)		/* del because of error */
{
	int			i;		/* btree level */

	/*
	 * Clear the buffer pointers and release the buffers. If we're doing
	 * this because of an error, inspect all of the entries in the bc_bufs
	 * array for buffers to be unlocked. This is because some of the btree
	 * code works from level n down to 0, and if we get an error along the
	 * way we won't have initialized all the entries down to 0.
	 */
	for (i = 0; i < cur->bc_nlevels; i++) {
		if (cur->bc_levels[i].bp)
			xfs_trans_brelse(cur->bc_tp, cur->bc_levels[i].bp);
		else if (!error)
			break;
	}

	/*
	 * If we are doing a BMBT update, the number of unaccounted blocks
	 * allocated during this cursor life time should be zero. If it's not
	 * zero, then we should be shut down or on our way to shutdown due to
	 * cancelling a dirty transaction on error.
	 */
	ASSERT(!xfs_btree_is_bmap(cur->bc_ops) || cur->bc_bmap.allocated == 0 ||
	       xfs_is_shutdown(cur->bc_mp) || error != 0);

	if (cur->bc_group)
		xfs_group_put(cur->bc_group);
	kmem_cache_free(cur->bc_cache, cur);
}

/* Return the buffer target for this btree's buffer. */
static inline struct xfs_buftarg *
xfs_btree_buftarg(
	struct xfs_btree_cur	*cur)
{
	if (cur->bc_ops->type == XFS_BTREE_TYPE_MEM)
		return cur->bc_mem.xfbtree->target;
	return cur->bc_mp->m_ddev_targp;
}

/* Return the block size (in units of 512b sectors) for this btree. */
static inline unsigned int
xfs_btree_bbsize(
	struct xfs_btree_cur	*cur)
{
	if (cur->bc_ops->type == XFS_BTREE_TYPE_MEM)
		return XFBNO_BBSIZE;
	return cur->bc_mp->m_bsize;
}

/*
 * Duplicate the btree cursor.
 * Allocate a new one, copy the record, re-get the buffers.
 */
int						/* error */
xfs_btree_dup_cursor(
	struct xfs_btree_cur	*cur,		/* input cursor */
	struct xfs_btree_cur	**ncur)		/* output cursor */
{
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_trans	*tp = cur->bc_tp;
	struct xfs_buf		*bp;
	struct xfs_btree_cur	*new;
	int			error;
	int			i;

	/*
	 * Don't allow staging cursors to be duplicated because they're supposed
	 * to be kept private to a single thread.
	 */
	if (unlikely(cur->bc_flags & XFS_BTREE_STAGING)) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	/*
	 * Allocate a new cursor like the old one.
	 */
	new = cur->bc_ops->dup_cursor(cur);

	/*
	 * Copy the record currently in the cursor.
	 */
	new->bc_rec = cur->bc_rec;

	/*
	 * For each level current, re-get the buffer and copy the ptr value.
	 */
	for (i = 0; i < new->bc_nlevels; i++) {
		new->bc_levels[i].ptr = cur->bc_levels[i].ptr;
		new->bc_levels[i].ra = cur->bc_levels[i].ra;
		bp = cur->bc_levels[i].bp;
		if (bp) {
			error = xfs_trans_read_buf(mp, tp,
					xfs_btree_buftarg(cur),
					xfs_buf_daddr(bp),
					xfs_btree_bbsize(cur), 0, &bp,
					cur->bc_ops->buf_ops);
			if (xfs_metadata_is_sick(error))
				xfs_btree_mark_sick(new);
			if (error) {
				xfs_btree_del_cursor(new, error);
				*ncur = NULL;
				return error;
			}
		}
		new->bc_levels[i].bp = bp;
	}
	*ncur = new;
	return 0;
}

/*
 * XFS btree block layout and addressing:
 *
 * There are two types of blocks in the btree: leaf and non-leaf blocks.
 *
 * The leaf record start with a header then followed by records containing
 * the values.  A non-leaf block also starts with the same header, and
 * then first contains lookup keys followed by an equal number of pointers
 * to the btree blocks at the previous level.
 *
 *		+--------+-------+-------+-------+-------+-------+-------+
 * Leaf:	| header | rec 1 | rec 2 | rec 3 | rec 4 | rec 5 | rec N |
 *		+--------+-------+-------+-------+-------+-------+-------+
 *
 *		+--------+-------+-------+-------+-------+-------+-------+
 * Non-Leaf:	| header | key 1 | key 2 | key N | ptr 1 | ptr 2 | ptr N |
 *		+--------+-------+-------+-------+-------+-------+-------+
 *
 * The header is called struct xfs_btree_block for reasons better left unknown
 * and comes in different versions for short (32bit) and long (64bit) block
 * pointers.  The record and key structures are defined by the btree instances
 * and opaque to the btree core.  The block pointers are simple disk endian
 * integers, available in a short (32bit) and long (64bit) variant.
 *
 * The helpers below calculate the offset of a given record, key or pointer
 * into a btree block (xfs_btree_*_offset) or return a pointer to the given
 * record, key or pointer (xfs_btree_*_addr).  Note that all addressing
 * inside the btree block is done using indices starting at one, not zero!
 *
 * If XFS_BTGEO_OVERLAPPING is set, then this btree supports keys containing
 * overlapping intervals.  In such a tree, records are still sorted lowest to
 * highest and indexed by the smallest key value that refers to the record.
 * However, nodes are different: each pointer has two associated keys -- one
 * indexing the lowest key available in the block(s) below (the same behavior
 * as the key in a regular btree) and another indexing the highest key
 * available in the block(s) below.  Because records are /not/ sorted by the
 * highest key, all leaf block updates require us to compute the highest key
 * that matches any record in the leaf and to recursively update the high keys
 * in the nodes going further up in the tree, if necessary.  Nodes look like
 * this:
 *
 *		+--------+-----+-----+-----+-----+-----+-------+-------+-----+
 * Non-Leaf:	| header | lo1 | hi1 | lo2 | hi2 | ... | ptr 1 | ptr 2 | ... |
 *		+--------+-----+-----+-----+-----+-----+-------+-------+-----+
 *
 * To perform an interval query on an overlapped tree, perform the usual
 * depth-first search and use the low and high keys to decide if we can skip
 * that particular node.  If a leaf node is reached, return the records that
 * intersect the interval.  Note that an interval query may return numerous
 * entries.  For a non-overlapped tree, simply search for the record associated
 * with the lowest key and iterate forward until a non-matching record is
 * found.  Section 14.3 ("Interval Trees") of _Introduction to Algorithms_ by
 * Cormen, Leiserson, Rivest, and Stein (2nd or 3rd ed. only) discuss this in
 * more detail.
 *
 * Why do we care about overlapping intervals?  Let's say you have a bunch of
 * reverse mapping records on a reflink filesystem:
 *
 * 1: +- file A startblock B offset C length D -----------+
 * 2:      +- file E startblock F offset G length H --------------+
 * 3:      +- file I startblock F offset J length K --+
 * 4:                                                        +- file L... --+
 *
 * Now say we want to map block (B+D) into file A at offset (C+D).  Ideally,
 * we'd simply increment the length of record 1.  But how do we find the record
 * that ends at (B+D-1) (i.e. record 1)?  A LE lookup of (B+D-1) would return
 * record 3 because the keys are ordered first by startblock.  An interval
 * query would return records 1 and 2 because they both overlap (B+D-1), and
 * from that we can pick out record 1 as the appropriate left neighbor.
 *
 * In the non-overlapped case you can do a LE lookup and decrement the cursor
 * because a record's interval must end before the next record.
 */

/*
 * Return size of the btree block header for this btree instance.
 */
static inline size_t xfs_btree_block_len(struct xfs_btree_cur *cur)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (xfs_has_crc(cur->bc_mp))
			return XFS_BTREE_LBLOCK_CRC_LEN;
		return XFS_BTREE_LBLOCK_LEN;
	}
	if (xfs_has_crc(cur->bc_mp))
		return XFS_BTREE_SBLOCK_CRC_LEN;
	return XFS_BTREE_SBLOCK_LEN;
}

/*
 * Calculate offset of the n-th record in a btree block.
 */
STATIC size_t
xfs_btree_rec_offset(
	struct xfs_btree_cur	*cur,
	int			n)
{
	return xfs_btree_block_len(cur) +
		(n - 1) * cur->bc_ops->rec_len;
}

/*
 * Calculate offset of the n-th key in a btree block.
 */
STATIC size_t
xfs_btree_key_offset(
	struct xfs_btree_cur	*cur,
	int			n)
{
	return xfs_btree_block_len(cur) +
		(n - 1) * cur->bc_ops->key_len;
}

/*
 * Calculate offset of the n-th high key in a btree block.
 */
STATIC size_t
xfs_btree_high_key_offset(
	struct xfs_btree_cur	*cur,
	int			n)
{
	return xfs_btree_block_len(cur) +
		(n - 1) * cur->bc_ops->key_len + (cur->bc_ops->key_len / 2);
}

/*
 * Calculate offset of the n-th block pointer in a btree block.
 */
STATIC size_t
xfs_btree_ptr_offset(
	struct xfs_btree_cur	*cur,
	int			n,
	int			level)
{
	return xfs_btree_block_len(cur) +
		cur->bc_ops->get_maxrecs(cur, level) * cur->bc_ops->key_len +
		(n - 1) * cur->bc_ops->ptr_len;
}

/*
 * Return a pointer to the n-th record in the btree block.
 */
union xfs_btree_rec *
xfs_btree_rec_addr(
	struct xfs_btree_cur	*cur,
	int			n,
	struct xfs_btree_block	*block)
{
	return (union xfs_btree_rec *)
		((char *)block + xfs_btree_rec_offset(cur, n));
}

/*
 * Return a pointer to the n-th key in the btree block.
 */
union xfs_btree_key *
xfs_btree_key_addr(
	struct xfs_btree_cur	*cur,
	int			n,
	struct xfs_btree_block	*block)
{
	return (union xfs_btree_key *)
		((char *)block + xfs_btree_key_offset(cur, n));
}

/*
 * Return a pointer to the n-th high key in the btree block.
 */
union xfs_btree_key *
xfs_btree_high_key_addr(
	struct xfs_btree_cur	*cur,
	int			n,
	struct xfs_btree_block	*block)
{
	return (union xfs_btree_key *)
		((char *)block + xfs_btree_high_key_offset(cur, n));
}

/*
 * Return a pointer to the n-th block pointer in the btree block.
 */
union xfs_btree_ptr *
xfs_btree_ptr_addr(
	struct xfs_btree_cur	*cur,
	int			n,
	struct xfs_btree_block	*block)
{
	int			level = xfs_btree_get_level(block);

	ASSERT(block->bb_level != 0);

	return (union xfs_btree_ptr *)
		((char *)block + xfs_btree_ptr_offset(cur, n, level));
}

struct xfs_ifork *
xfs_btree_ifork_ptr(
	struct xfs_btree_cur	*cur)
{
	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_INODE);

	if (cur->bc_flags & XFS_BTREE_STAGING)
		return cur->bc_ino.ifake->if_fork;
	return xfs_ifork_ptr(cur->bc_ino.ip, cur->bc_ino.whichfork);
}

/*
 * Get the root block which is stored in the inode.
 *
 * For now this btree implementation assumes the btree root is always
 * stored in the if_broot field of an inode fork.
 */
STATIC struct xfs_btree_block *
xfs_btree_get_iroot(
	struct xfs_btree_cur	*cur)
{
	struct xfs_ifork	*ifp = xfs_btree_ifork_ptr(cur);

	return (struct xfs_btree_block *)ifp->if_broot;
}

/*
 * Retrieve the block pointer from the cursor at the given level.
 * This may be an inode btree root or from a buffer.
 */
struct xfs_btree_block *		/* generic btree block pointer */
xfs_btree_get_block(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			level,	/* level in btree */
	struct xfs_buf		**bpp)	/* buffer containing the block */
{
	if (xfs_btree_at_iroot(cur, level)) {
		*bpp = NULL;
		return xfs_btree_get_iroot(cur);
	}

	*bpp = cur->bc_levels[level].bp;
	return XFS_BUF_TO_BLOCK(*bpp);
}

/*
 * Change the cursor to point to the first record at the given level.
 * Other levels are unaffected.
 */
STATIC int				/* success=1, failure=0 */
xfs_btree_firstrec(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			level)	/* level to change */
{
	struct xfs_btree_block	*block;	/* generic btree block pointer */
	struct xfs_buf		*bp;	/* buffer containing block */

	/*
	 * Get the block pointer for this level.
	 */
	block = xfs_btree_get_block(cur, level, &bp);
	if (xfs_btree_check_block(cur, block, level, bp))
		return 0;
	/*
	 * It's empty, there is no such record.
	 */
	if (!block->bb_numrecs)
		return 0;
	/*
	 * Set the ptr value to 1, that's the first record/key.
	 */
	cur->bc_levels[level].ptr = 1;
	return 1;
}

/*
 * Change the cursor to point to the last record in the current block
 * at the given level.  Other levels are unaffected.
 */
STATIC int				/* success=1, failure=0 */
xfs_btree_lastrec(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			level)	/* level to change */
{
	struct xfs_btree_block	*block;	/* generic btree block pointer */
	struct xfs_buf		*bp;	/* buffer containing block */

	/*
	 * Get the block pointer for this level.
	 */
	block = xfs_btree_get_block(cur, level, &bp);
	if (xfs_btree_check_block(cur, block, level, bp))
		return 0;
	/*
	 * It's empty, there is no such record.
	 */
	if (!block->bb_numrecs)
		return 0;
	/*
	 * Set the ptr value to numrecs, that's the last record/key.
	 */
	cur->bc_levels[level].ptr = be16_to_cpu(block->bb_numrecs);
	return 1;
}

/*
 * Compute first and last byte offsets for the fields given.
 * Interprets the offsets table, which contains struct field offsets.
 */
void
xfs_btree_offsets(
	uint32_t	fields,		/* bitmask of fields */
	const short	*offsets,	/* table of field offsets */
	int		nbits,		/* number of bits to inspect */
	int		*first,		/* output: first byte offset */
	int		*last)		/* output: last byte offset */
{
	int		i;		/* current bit number */
	uint32_t	imask;		/* mask for current bit number */

	ASSERT(fields != 0);
	/*
	 * Find the lowest bit, so the first byte offset.
	 */
	for (i = 0, imask = 1u; ; i++, imask <<= 1) {
		if (imask & fields) {
			*first = offsets[i];
			break;
		}
	}
	/*
	 * Find the highest bit, so the last byte offset.
	 */
	for (i = nbits - 1, imask = 1u << i; ; i--, imask >>= 1) {
		if (imask & fields) {
			*last = offsets[i + 1] - 1;
			break;
		}
	}
}

STATIC int
xfs_btree_readahead_fsblock(
	struct xfs_btree_cur	*cur,
	int			lr,
	struct xfs_btree_block	*block)
{
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_fsblock_t		left = be64_to_cpu(block->bb_u.l.bb_leftsib);
	xfs_fsblock_t		right = be64_to_cpu(block->bb_u.l.bb_rightsib);
	int			rval = 0;

	if ((lr & XFS_BTCUR_LEFTRA) && left != NULLFSBLOCK) {
		xfs_buf_readahead(mp->m_ddev_targp, XFS_FSB_TO_DADDR(mp, left),
				mp->m_bsize, cur->bc_ops->buf_ops);
		rval++;
	}

	if ((lr & XFS_BTCUR_RIGHTRA) && right != NULLFSBLOCK) {
		xfs_buf_readahead(mp->m_ddev_targp, XFS_FSB_TO_DADDR(mp, right),
				mp->m_bsize, cur->bc_ops->buf_ops);
		rval++;
	}

	return rval;
}

STATIC int
xfs_btree_readahead_memblock(
	struct xfs_btree_cur	*cur,
	int			lr,
	struct xfs_btree_block	*block)
{
	struct xfs_buftarg	*btp = cur->bc_mem.xfbtree->target;
	xfbno_t			left = be64_to_cpu(block->bb_u.l.bb_leftsib);
	xfbno_t			right = be64_to_cpu(block->bb_u.l.bb_rightsib);
	int			rval = 0;

	if ((lr & XFS_BTCUR_LEFTRA) && left != NULLFSBLOCK) {
		xfs_buf_readahead(btp, xfbno_to_daddr(left), XFBNO_BBSIZE,
				cur->bc_ops->buf_ops);
		rval++;
	}

	if ((lr & XFS_BTCUR_RIGHTRA) && right != NULLFSBLOCK) {
		xfs_buf_readahead(btp, xfbno_to_daddr(right), XFBNO_BBSIZE,
				cur->bc_ops->buf_ops);
		rval++;
	}

	return rval;
}

STATIC int
xfs_btree_readahead_agblock(
	struct xfs_btree_cur	*cur,
	int			lr,
	struct xfs_btree_block	*block)
{
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_perag	*pag = to_perag(cur->bc_group);
	xfs_agblock_t		left = be32_to_cpu(block->bb_u.s.bb_leftsib);
	xfs_agblock_t		right = be32_to_cpu(block->bb_u.s.bb_rightsib);
	int			rval = 0;

	if ((lr & XFS_BTCUR_LEFTRA) && left != NULLAGBLOCK) {
		xfs_buf_readahead(mp->m_ddev_targp,
				xfs_agbno_to_daddr(pag, left), mp->m_bsize,
				cur->bc_ops->buf_ops);
		rval++;
	}

	if ((lr & XFS_BTCUR_RIGHTRA) && right != NULLAGBLOCK) {
		xfs_buf_readahead(mp->m_ddev_targp,
				xfs_agbno_to_daddr(pag, right), mp->m_bsize,
				cur->bc_ops->buf_ops);
		rval++;
	}

	return rval;
}

/*
 * Read-ahead btree blocks, at the given level.
 * Bits in lr are set from XFS_BTCUR_{LEFT,RIGHT}RA.
 */
STATIC int
xfs_btree_readahead(
	struct xfs_btree_cur	*cur,		/* btree cursor */
	int			lev,		/* level in btree */
	int			lr)		/* left/right bits */
{
	struct xfs_btree_block	*block;

	/*
	 * No readahead needed if we are at the root level and the
	 * btree root is stored in the inode.
	 */
	if (xfs_btree_at_iroot(cur, lev))
		return 0;

	if ((cur->bc_levels[lev].ra | lr) == cur->bc_levels[lev].ra)
		return 0;

	cur->bc_levels[lev].ra |= lr;
	block = XFS_BUF_TO_BLOCK(cur->bc_levels[lev].bp);

	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_AG:
		return xfs_btree_readahead_agblock(cur, lr, block);
	case XFS_BTREE_TYPE_INODE:
		return xfs_btree_readahead_fsblock(cur, lr, block);
	case XFS_BTREE_TYPE_MEM:
		return xfs_btree_readahead_memblock(cur, lr, block);
	default:
		ASSERT(0);
		return 0;
	}
}

STATIC int
xfs_btree_ptr_to_daddr(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	xfs_daddr_t			*daddr)
{
	int			error;

	error = xfs_btree_check_ptr(cur, ptr, 0, 1);
	if (error)
		return error;

	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_AG:
		*daddr = xfs_agbno_to_daddr(to_perag(cur->bc_group),
				be32_to_cpu(ptr->s));
		break;
	case XFS_BTREE_TYPE_INODE:
		*daddr = XFS_FSB_TO_DADDR(cur->bc_mp, be64_to_cpu(ptr->l));
		break;
	case XFS_BTREE_TYPE_MEM:
		*daddr = xfbno_to_daddr(be64_to_cpu(ptr->l));
		break;
	}
	return 0;
}

/*
 * Readahead @count btree blocks at the given @ptr location.
 *
 * We don't need to care about long or short form btrees here as we have a
 * method of converting the ptr directly to a daddr available to us.
 */
STATIC void
xfs_btree_readahead_ptr(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	xfs_extlen_t		count)
{
	xfs_daddr_t		daddr;

	if (xfs_btree_ptr_to_daddr(cur, ptr, &daddr))
		return;
	xfs_buf_readahead(xfs_btree_buftarg(cur), daddr,
			xfs_btree_bbsize(cur) * count,
			cur->bc_ops->buf_ops);
}

/*
 * Set the buffer for level "lev" in the cursor to bp, releasing
 * any previous buffer.
 */
STATIC void
xfs_btree_setbuf(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			lev,	/* level in btree */
	struct xfs_buf		*bp)	/* new buffer to set */
{
	struct xfs_btree_block	*b;	/* btree block */

	if (cur->bc_levels[lev].bp)
		xfs_trans_brelse(cur->bc_tp, cur->bc_levels[lev].bp);
	cur->bc_levels[lev].bp = bp;
	cur->bc_levels[lev].ra = 0;

	b = XFS_BUF_TO_BLOCK(bp);
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (b->bb_u.l.bb_leftsib == cpu_to_be64(NULLFSBLOCK))
			cur->bc_levels[lev].ra |= XFS_BTCUR_LEFTRA;
		if (b->bb_u.l.bb_rightsib == cpu_to_be64(NULLFSBLOCK))
			cur->bc_levels[lev].ra |= XFS_BTCUR_RIGHTRA;
	} else {
		if (b->bb_u.s.bb_leftsib == cpu_to_be32(NULLAGBLOCK))
			cur->bc_levels[lev].ra |= XFS_BTCUR_LEFTRA;
		if (b->bb_u.s.bb_rightsib == cpu_to_be32(NULLAGBLOCK))
			cur->bc_levels[lev].ra |= XFS_BTCUR_RIGHTRA;
	}
}

bool
xfs_btree_ptr_is_null(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		return ptr->l == cpu_to_be64(NULLFSBLOCK);
	else
		return ptr->s == cpu_to_be32(NULLAGBLOCK);
}

void
xfs_btree_set_ptr_null(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		ptr->l = cpu_to_be64(NULLFSBLOCK);
	else
		ptr->s = cpu_to_be32(NULLAGBLOCK);
}

static inline bool
xfs_btree_ptrs_equal(
	struct xfs_btree_cur		*cur,
	union xfs_btree_ptr		*ptr1,
	union xfs_btree_ptr		*ptr2)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		return ptr1->l == ptr2->l;
	return ptr1->s == ptr2->s;
}

/*
 * Get/set/init sibling pointers
 */
void
xfs_btree_get_sibling(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	union xfs_btree_ptr	*ptr,
	int			lr)
{
	ASSERT(lr == XFS_BB_LEFTSIB || lr == XFS_BB_RIGHTSIB);

	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (lr == XFS_BB_RIGHTSIB)
			ptr->l = block->bb_u.l.bb_rightsib;
		else
			ptr->l = block->bb_u.l.bb_leftsib;
	} else {
		if (lr == XFS_BB_RIGHTSIB)
			ptr->s = block->bb_u.s.bb_rightsib;
		else
			ptr->s = block->bb_u.s.bb_leftsib;
	}
}

void
xfs_btree_set_sibling(
	struct xfs_btree_cur		*cur,
	struct xfs_btree_block		*block,
	const union xfs_btree_ptr	*ptr,
	int				lr)
{
	ASSERT(lr == XFS_BB_LEFTSIB || lr == XFS_BB_RIGHTSIB);

	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (lr == XFS_BB_RIGHTSIB)
			block->bb_u.l.bb_rightsib = ptr->l;
		else
			block->bb_u.l.bb_leftsib = ptr->l;
	} else {
		if (lr == XFS_BB_RIGHTSIB)
			block->bb_u.s.bb_rightsib = ptr->s;
		else
			block->bb_u.s.bb_leftsib = ptr->s;
	}
}

static void
__xfs_btree_init_block(
	struct xfs_mount	*mp,
	struct xfs_btree_block	*buf,
	const struct xfs_btree_ops *ops,
	xfs_daddr_t		blkno,
	__u16			level,
	__u16			numrecs,
	__u64			owner)
{
	bool			crc = xfs_has_crc(mp);
	__u32			magic = xfs_btree_magic(mp, ops);

	buf->bb_magic = cpu_to_be32(magic);
	buf->bb_level = cpu_to_be16(level);
	buf->bb_numrecs = cpu_to_be16(numrecs);

	if (ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		buf->bb_u.l.bb_leftsib = cpu_to_be64(NULLFSBLOCK);
		buf->bb_u.l.bb_rightsib = cpu_to_be64(NULLFSBLOCK);
		if (crc) {
			buf->bb_u.l.bb_blkno = cpu_to_be64(blkno);
			buf->bb_u.l.bb_owner = cpu_to_be64(owner);
			uuid_copy(&buf->bb_u.l.bb_uuid, &mp->m_sb.sb_meta_uuid);
			buf->bb_u.l.bb_pad = 0;
			buf->bb_u.l.bb_lsn = 0;
		}
	} else {
		buf->bb_u.s.bb_leftsib = cpu_to_be32(NULLAGBLOCK);
		buf->bb_u.s.bb_rightsib = cpu_to_be32(NULLAGBLOCK);
		if (crc) {
			buf->bb_u.s.bb_blkno = cpu_to_be64(blkno);
			/* owner is a 32 bit value on short blocks */
			buf->bb_u.s.bb_owner = cpu_to_be32((__u32)owner);
			uuid_copy(&buf->bb_u.s.bb_uuid, &mp->m_sb.sb_meta_uuid);
			buf->bb_u.s.bb_lsn = 0;
		}
	}
}

void
xfs_btree_init_block(
	struct xfs_mount	*mp,
	struct xfs_btree_block	*block,
	const struct xfs_btree_ops *ops,
	__u16			level,
	__u16			numrecs,
	__u64			owner)
{
	__xfs_btree_init_block(mp, block, ops, XFS_BUF_DADDR_NULL, level,
			numrecs, owner);
}

void
xfs_btree_init_buf(
	struct xfs_mount		*mp,
	struct xfs_buf			*bp,
	const struct xfs_btree_ops	*ops,
	__u16				level,
	__u16				numrecs,
	__u64				owner)
{
	__xfs_btree_init_block(mp, XFS_BUF_TO_BLOCK(bp), ops,
			xfs_buf_daddr(bp), level, numrecs, owner);
	bp->b_ops = ops->buf_ops;
}

static inline __u64
xfs_btree_owner(
	struct xfs_btree_cur    *cur)
{
	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_MEM:
		return cur->bc_mem.xfbtree->owner;
	case XFS_BTREE_TYPE_INODE:
		return cur->bc_ino.ip->i_ino;
	case XFS_BTREE_TYPE_AG:
		return cur->bc_group->xg_gno;
	default:
		ASSERT(0);
		return 0;
	}
}

void
xfs_btree_init_block_cur(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	int			level,
	int			numrecs)
{
	xfs_btree_init_buf(cur->bc_mp, bp, cur->bc_ops, level, numrecs,
			xfs_btree_owner(cur));
}

STATIC void
xfs_btree_buf_to_ptr(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	union xfs_btree_ptr	*ptr)
{
	switch (cur->bc_ops->type) {
	case XFS_BTREE_TYPE_AG:
		ptr->s = cpu_to_be32(xfs_daddr_to_agbno(cur->bc_mp,
					xfs_buf_daddr(bp)));
		break;
	case XFS_BTREE_TYPE_INODE:
		ptr->l = cpu_to_be64(XFS_DADDR_TO_FSB(cur->bc_mp,
					xfs_buf_daddr(bp)));
		break;
	case XFS_BTREE_TYPE_MEM:
		ptr->l = cpu_to_be64(xfs_daddr_to_xfbno(xfs_buf_daddr(bp)));
		break;
	}
}

static inline void
xfs_btree_set_refs(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	xfs_buf_set_ref(bp, cur->bc_ops->lru_refs);
}

int
xfs_btree_get_buf_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	struct xfs_btree_block		**block,
	struct xfs_buf			**bpp)
{
	xfs_daddr_t			d;
	int				error;

	error = xfs_btree_ptr_to_daddr(cur, ptr, &d);
	if (error)
		return error;
	error = xfs_trans_get_buf(cur->bc_tp, xfs_btree_buftarg(cur), d,
			xfs_btree_bbsize(cur), 0, bpp);
	if (error)
		return error;

	(*bpp)->b_ops = cur->bc_ops->buf_ops;
	*block = XFS_BUF_TO_BLOCK(*bpp);
	return 0;
}

/*
 * Read in the buffer at the given ptr and return the buffer and
 * the block pointer within the buffer.
 */
int
xfs_btree_read_buf_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				flags,
	struct xfs_btree_block		**block,
	struct xfs_buf			**bpp)
{
	struct xfs_mount	*mp = cur->bc_mp;
	xfs_daddr_t		d;
	int			error;

	/* need to sort out how callers deal with failures first */
	ASSERT(!(flags & XBF_TRYLOCK));

	error = xfs_btree_ptr_to_daddr(cur, ptr, &d);
	if (error)
		return error;
	error = xfs_trans_read_buf(mp, cur->bc_tp, xfs_btree_buftarg(cur), d,
			xfs_btree_bbsize(cur), flags, bpp,
			cur->bc_ops->buf_ops);
	if (xfs_metadata_is_sick(error))
		xfs_btree_mark_sick(cur);
	if (error)
		return error;

	xfs_btree_set_refs(cur, *bpp);
	*block = XFS_BUF_TO_BLOCK(*bpp);
	return 0;
}

/*
 * Copy keys from one btree block to another.
 */
void
xfs_btree_copy_keys(
	struct xfs_btree_cur		*cur,
	union xfs_btree_key		*dst_key,
	const union xfs_btree_key	*src_key,
	int				numkeys)
{
	ASSERT(numkeys >= 0);
	memcpy(dst_key, src_key, numkeys * cur->bc_ops->key_len);
}

/*
 * Copy records from one btree block to another.
 */
STATIC void
xfs_btree_copy_recs(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*dst_rec,
	union xfs_btree_rec	*src_rec,
	int			numrecs)
{
	ASSERT(numrecs >= 0);
	memcpy(dst_rec, src_rec, numrecs * cur->bc_ops->rec_len);
}

/*
 * Copy block pointers from one btree block to another.
 */
void
xfs_btree_copy_ptrs(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*dst_ptr,
	const union xfs_btree_ptr *src_ptr,
	int			numptrs)
{
	ASSERT(numptrs >= 0);
	memcpy(dst_ptr, src_ptr, numptrs * cur->bc_ops->ptr_len);
}

/*
 * Shift keys one index left/right inside a single btree block.
 */
STATIC void
xfs_btree_shift_keys(
	struct xfs_btree_cur	*cur,
	union xfs_btree_key	*key,
	int			dir,
	int			numkeys)
{
	char			*dst_key;

	ASSERT(numkeys >= 0);
	ASSERT(dir == 1 || dir == -1);

	dst_key = (char *)key + (dir * cur->bc_ops->key_len);
	memmove(dst_key, key, numkeys * cur->bc_ops->key_len);
}

/*
 * Shift records one index left/right inside a single btree block.
 */
STATIC void
xfs_btree_shift_recs(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec,
	int			dir,
	int			numrecs)
{
	char			*dst_rec;

	ASSERT(numrecs >= 0);
	ASSERT(dir == 1 || dir == -1);

	dst_rec = (char *)rec + (dir * cur->bc_ops->rec_len);
	memmove(dst_rec, rec, numrecs * cur->bc_ops->rec_len);
}

/*
 * Shift block pointers one index left/right inside a single btree block.
 */
STATIC void
xfs_btree_shift_ptrs(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	int			dir,
	int			numptrs)
{
	char			*dst_ptr;

	ASSERT(numptrs >= 0);
	ASSERT(dir == 1 || dir == -1);

	dst_ptr = (char *)ptr + (dir * cur->bc_ops->ptr_len);
	memmove(dst_ptr, ptr, numptrs * cur->bc_ops->ptr_len);
}

/*
 * Log key values from the btree block.
 */
STATIC void
xfs_btree_log_keys(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	int			first,
	int			last)
{

	if (bp) {
		xfs_trans_buf_set_type(cur->bc_tp, bp, XFS_BLFT_BTREE_BUF);
		xfs_trans_log_buf(cur->bc_tp, bp,
				  xfs_btree_key_offset(cur, first),
				  xfs_btree_key_offset(cur, last + 1) - 1);
	} else {
		xfs_trans_log_inode(cur->bc_tp, cur->bc_ino.ip,
				xfs_ilog_fbroot(cur->bc_ino.whichfork));
	}
}

/*
 * Log record values from the btree block.
 */
void
xfs_btree_log_recs(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	int			first,
	int			last)
{
	if (!bp) {
		xfs_trans_log_inode(cur->bc_tp, cur->bc_ino.ip,
				xfs_ilog_fbroot(cur->bc_ino.whichfork));
		return;
	}

	xfs_trans_buf_set_type(cur->bc_tp, bp, XFS_BLFT_BTREE_BUF);
	xfs_trans_log_buf(cur->bc_tp, bp,
			  xfs_btree_rec_offset(cur, first),
			  xfs_btree_rec_offset(cur, last + 1) - 1);
}

/*
 * Log block pointer fields from a btree block (nonleaf).
 */
STATIC void
xfs_btree_log_ptrs(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	struct xfs_buf		*bp,	/* buffer containing btree block */
	int			first,	/* index of first pointer to log */
	int			last)	/* index of last pointer to log */
{

	if (bp) {
		struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
		int			level = xfs_btree_get_level(block);

		xfs_trans_buf_set_type(cur->bc_tp, bp, XFS_BLFT_BTREE_BUF);
		xfs_trans_log_buf(cur->bc_tp, bp,
				xfs_btree_ptr_offset(cur, first, level),
				xfs_btree_ptr_offset(cur, last + 1, level) - 1);
	} else {
		xfs_trans_log_inode(cur->bc_tp, cur->bc_ino.ip,
			xfs_ilog_fbroot(cur->bc_ino.whichfork));
	}

}

/*
 * Log fields from a btree block header.
 */
void
xfs_btree_log_block(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	struct xfs_buf		*bp,	/* buffer containing btree block */
	uint32_t		fields)	/* mask of fields: XFS_BB_... */
{
	int			first;	/* first byte offset logged */
	int			last;	/* last byte offset logged */
	static const short	soffsets[] = {	/* table of offsets (short) */
		offsetof(struct xfs_btree_block, bb_magic),
		offsetof(struct xfs_btree_block, bb_level),
		offsetof(struct xfs_btree_block, bb_numrecs),
		offsetof(struct xfs_btree_block, bb_u.s.bb_leftsib),
		offsetof(struct xfs_btree_block, bb_u.s.bb_rightsib),
		offsetof(struct xfs_btree_block, bb_u.s.bb_blkno),
		offsetof(struct xfs_btree_block, bb_u.s.bb_lsn),
		offsetof(struct xfs_btree_block, bb_u.s.bb_uuid),
		offsetof(struct xfs_btree_block, bb_u.s.bb_owner),
		offsetof(struct xfs_btree_block, bb_u.s.bb_crc),
		XFS_BTREE_SBLOCK_CRC_LEN
	};
	static const short	loffsets[] = {	/* table of offsets (long) */
		offsetof(struct xfs_btree_block, bb_magic),
		offsetof(struct xfs_btree_block, bb_level),
		offsetof(struct xfs_btree_block, bb_numrecs),
		offsetof(struct xfs_btree_block, bb_u.l.bb_leftsib),
		offsetof(struct xfs_btree_block, bb_u.l.bb_rightsib),
		offsetof(struct xfs_btree_block, bb_u.l.bb_blkno),
		offsetof(struct xfs_btree_block, bb_u.l.bb_lsn),
		offsetof(struct xfs_btree_block, bb_u.l.bb_uuid),
		offsetof(struct xfs_btree_block, bb_u.l.bb_owner),
		offsetof(struct xfs_btree_block, bb_u.l.bb_crc),
		offsetof(struct xfs_btree_block, bb_u.l.bb_pad),
		XFS_BTREE_LBLOCK_CRC_LEN
	};

	if (bp) {
		int nbits;

		if (xfs_has_crc(cur->bc_mp)) {
			/*
			 * We don't log the CRC when updating a btree
			 * block but instead recreate it during log
			 * recovery.  As the log buffers have checksums
			 * of their own this is safe and avoids logging a crc
			 * update in a lot of places.
			 */
			if (fields == XFS_BB_ALL_BITS)
				fields = XFS_BB_ALL_BITS_CRC;
			nbits = XFS_BB_NUM_BITS_CRC;
		} else {
			nbits = XFS_BB_NUM_BITS;
		}
		xfs_btree_offsets(fields,
				  (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) ?
					loffsets : soffsets,
				  nbits, &first, &last);
		xfs_trans_buf_set_type(cur->bc_tp, bp, XFS_BLFT_BTREE_BUF);
		xfs_trans_log_buf(cur->bc_tp, bp, first, last);
	} else {
		xfs_trans_log_inode(cur->bc_tp, cur->bc_ino.ip,
			xfs_ilog_fbroot(cur->bc_ino.whichfork));
	}
}

/*
 * Increment cursor by one record at the level.
 * For nonzero levels the leaf-ward information is untouched.
 */
int						/* error */
xfs_btree_increment(
	struct xfs_btree_cur	*cur,
	int			level,
	int			*stat)		/* success/failure */
{
	struct xfs_btree_block	*block;
	union xfs_btree_ptr	ptr;
	struct xfs_buf		*bp;
	int			error;		/* error return value */
	int			lev;

	ASSERT(level < cur->bc_nlevels);

	/* Read-ahead to the right at this level. */
	xfs_btree_readahead(cur, level, XFS_BTCUR_RIGHTRA);

	/* Get a pointer to the btree block. */
	block = xfs_btree_get_block(cur, level, &bp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto error0;
#endif

	/* We're done if we remain in the block after the increment. */
	if (++cur->bc_levels[level].ptr <= xfs_btree_get_numrecs(block))
		goto out1;

	/* Fail if we just went off the right edge of the tree. */
	xfs_btree_get_sibling(cur, block, &ptr, XFS_BB_RIGHTSIB);
	if (xfs_btree_ptr_is_null(cur, &ptr))
		goto out0;

	XFS_BTREE_STATS_INC(cur, increment);

	/*
	 * March up the tree incrementing pointers.
	 * Stop when we don't go off the right edge of a block.
	 */
	for (lev = level + 1; lev < cur->bc_nlevels; lev++) {
		block = xfs_btree_get_block(cur, lev, &bp);

#ifdef DEBUG
		error = xfs_btree_check_block(cur, block, lev, bp);
		if (error)
			goto error0;
#endif

		if (++cur->bc_levels[lev].ptr <= xfs_btree_get_numrecs(block))
			break;

		/* Read-ahead the right block for the next loop. */
		xfs_btree_readahead(cur, lev, XFS_BTCUR_RIGHTRA);
	}

	/*
	 * If we went off the root then we are either seriously
	 * confused or have the tree root in an inode.
	 */
	if (lev == cur->bc_nlevels) {
		if (cur->bc_ops->type == XFS_BTREE_TYPE_INODE)
			goto out0;
		ASSERT(0);
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto error0;
	}
	ASSERT(lev < cur->bc_nlevels);

	/*
	 * Now walk back down the tree, fixing up the cursor's buffer
	 * pointers and key numbers.
	 */
	for (block = xfs_btree_get_block(cur, lev, &bp); lev > level; ) {
		union xfs_btree_ptr	*ptrp;

		ptrp = xfs_btree_ptr_addr(cur, cur->bc_levels[lev].ptr, block);
		--lev;
		error = xfs_btree_read_buf_block(cur, ptrp, 0, &block, &bp);
		if (error)
			goto error0;

		xfs_btree_setbuf(cur, lev, bp);
		cur->bc_levels[lev].ptr = 1;
	}
out1:
	*stat = 1;
	return 0;

out0:
	*stat = 0;
	return 0;

error0:
	return error;
}

/*
 * Decrement cursor by one record at the level.
 * For nonzero levels the leaf-ward information is untouched.
 */
int						/* error */
xfs_btree_decrement(
	struct xfs_btree_cur	*cur,
	int			level,
	int			*stat)		/* success/failure */
{
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;
	int			error;		/* error return value */
	int			lev;
	union xfs_btree_ptr	ptr;

	ASSERT(level < cur->bc_nlevels);

	/* Read-ahead to the left at this level. */
	xfs_btree_readahead(cur, level, XFS_BTCUR_LEFTRA);

	/* We're done if we remain in the block after the decrement. */
	if (--cur->bc_levels[level].ptr > 0)
		goto out1;

	/* Get a pointer to the btree block. */
	block = xfs_btree_get_block(cur, level, &bp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto error0;
#endif

	/* Fail if we just went off the left edge of the tree. */
	xfs_btree_get_sibling(cur, block, &ptr, XFS_BB_LEFTSIB);
	if (xfs_btree_ptr_is_null(cur, &ptr))
		goto out0;

	XFS_BTREE_STATS_INC(cur, decrement);

	/*
	 * March up the tree decrementing pointers.
	 * Stop when we don't go off the left edge of a block.
	 */
	for (lev = level + 1; lev < cur->bc_nlevels; lev++) {
		if (--cur->bc_levels[lev].ptr > 0)
			break;
		/* Read-ahead the left block for the next loop. */
		xfs_btree_readahead(cur, lev, XFS_BTCUR_LEFTRA);
	}

	/*
	 * If we went off the root then we are seriously confused.
	 * or the root of the tree is in an inode.
	 */
	if (lev == cur->bc_nlevels) {
		if (cur->bc_ops->type == XFS_BTREE_TYPE_INODE)
			goto out0;
		ASSERT(0);
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto error0;
	}
	ASSERT(lev < cur->bc_nlevels);

	/*
	 * Now walk back down the tree, fixing up the cursor's buffer
	 * pointers and key numbers.
	 */
	for (block = xfs_btree_get_block(cur, lev, &bp); lev > level; ) {
		union xfs_btree_ptr	*ptrp;

		ptrp = xfs_btree_ptr_addr(cur, cur->bc_levels[lev].ptr, block);
		--lev;
		error = xfs_btree_read_buf_block(cur, ptrp, 0, &block, &bp);
		if (error)
			goto error0;
		xfs_btree_setbuf(cur, lev, bp);
		cur->bc_levels[lev].ptr = xfs_btree_get_numrecs(block);
	}
out1:
	*stat = 1;
	return 0;

out0:
	*stat = 0;
	return 0;

error0:
	return error;
}

/*
 * Check the btree block owner now that we have the context to know who the
 * real owner is.
 */
static inline xfs_failaddr_t
xfs_btree_check_block_owner(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block)
{
	__u64			owner;

	if (!xfs_has_crc(cur->bc_mp) ||
	    (cur->bc_flags & XFS_BTREE_BMBT_INVALID_OWNER))
		return NULL;

	owner = xfs_btree_owner(cur);
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (be64_to_cpu(block->bb_u.l.bb_owner) != owner)
			return __this_address;
	} else {
		if (be32_to_cpu(block->bb_u.s.bb_owner) != owner)
			return __this_address;
	}

	return NULL;
}

int
xfs_btree_lookup_get_block(
	struct xfs_btree_cur		*cur,	/* btree cursor */
	int				level,	/* level in the btree */
	const union xfs_btree_ptr	*pp,	/* ptr to btree block */
	struct xfs_btree_block		**blkp) /* return btree block */
{
	struct xfs_buf		*bp;	/* buffer pointer for btree block */
	xfs_daddr_t		daddr;
	int			error = 0;

	/* special case the root block if in an inode */
	if (xfs_btree_at_iroot(cur, level)) {
		*blkp = xfs_btree_get_iroot(cur);
		return 0;
	}

	/*
	 * If the old buffer at this level for the disk address we are
	 * looking for re-use it.
	 *
	 * Otherwise throw it away and get a new one.
	 */
	bp = cur->bc_levels[level].bp;
	error = xfs_btree_ptr_to_daddr(cur, pp, &daddr);
	if (error)
		return error;
	if (bp && xfs_buf_daddr(bp) == daddr) {
		*blkp = XFS_BUF_TO_BLOCK(bp);
		return 0;
	}

	error = xfs_btree_read_buf_block(cur, pp, 0, blkp, &bp);
	if (error)
		return error;

	/* Check the inode owner since the verifiers don't. */
	if (xfs_btree_check_block_owner(cur, *blkp) != NULL)
		goto out_bad;

	/* Did we get the level we were looking for? */
	if (be16_to_cpu((*blkp)->bb_level) != level)
		goto out_bad;

	/* Check that internal nodes have at least one record. */
	if (level != 0 && be16_to_cpu((*blkp)->bb_numrecs) == 0)
		goto out_bad;

	xfs_btree_setbuf(cur, level, bp);
	return 0;

out_bad:
	*blkp = NULL;
	xfs_buf_mark_corrupt(bp);
	xfs_trans_brelse(cur->bc_tp, bp);
	xfs_btree_mark_sick(cur);
	return -EFSCORRUPTED;
}

/*
 * Get current search key.  For level 0 we don't actually have a key
 * structure so we make one up from the record.  For all other levels
 * we just return the right key.
 */
STATIC union xfs_btree_key *
xfs_lookup_get_search_key(
	struct xfs_btree_cur	*cur,
	int			level,
	int			keyno,
	struct xfs_btree_block	*block,
	union xfs_btree_key	*kp)
{
	if (level == 0) {
		cur->bc_ops->init_key_from_rec(kp,
				xfs_btree_rec_addr(cur, keyno, block));
		return kp;
	}

	return xfs_btree_key_addr(cur, keyno, block);
}

/*
 * Initialize a pointer to the root block.
 */
void
xfs_btree_init_ptr_from_cur(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr)
{
	if (cur->bc_ops->type == XFS_BTREE_TYPE_INODE) {
		/*
		 * Inode-rooted btrees call xfs_btree_get_iroot to find the root
		 * in xfs_btree_lookup_get_block and don't need a pointer here.
		 */
		ptr->l = 0;
	} else if (cur->bc_flags & XFS_BTREE_STAGING) {
		ptr->s = cpu_to_be32(cur->bc_ag.afake->af_root);
	} else {
		cur->bc_ops->init_ptr_from_cur(cur, ptr);
	}
}

/*
 * Lookup the record.  The cursor is made to point to it, based on dir.
 * stat is set to 0 if can't find any such record, 1 for success.
 */
int					/* error */
xfs_btree_lookup(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	xfs_lookup_t		dir,	/* <=, ==, or >= */
	int			*stat)	/* success/failure */
{
	struct xfs_btree_block	*block;	/* current btree block */
	int			cmp_r;	/* current key comparison result */
	int			error;	/* error return value */
	int			keyno;	/* current key number */
	int			level;	/* level in the btree */
	union xfs_btree_ptr	*pp;	/* ptr to btree block */
	union xfs_btree_ptr	ptr;	/* ptr to btree block */

	XFS_BTREE_STATS_INC(cur, lookup);

	/* No such thing as a zero-level tree. */
	if (XFS_IS_CORRUPT(cur->bc_mp, cur->bc_nlevels == 0)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	block = NULL;
	keyno = 0;

	/* initialise start pointer from cursor */
	xfs_btree_init_ptr_from_cur(cur, &ptr);
	pp = &ptr;

	/*
	 * Iterate over each level in the btree, starting at the root.
	 * For each level above the leaves, find the key we need, based
	 * on the lookup record, then follow the corresponding block
	 * pointer down to the next level.
	 */
	for (level = cur->bc_nlevels - 1, cmp_r = 1; level >= 0; level--) {
		/* Get the block we need to do the lookup on. */
		error = xfs_btree_lookup_get_block(cur, level, pp, &block);
		if (error)
			goto error0;

		if (cmp_r == 0) {
			/*
			 * If we already had a key match at a higher level, we
			 * know we need to use the first entry in this block.
			 */
			keyno = 1;
		} else {
			/* Otherwise search this block. Do a binary search. */

			int	high;	/* high entry number */
			int	low;	/* low entry number */

			/* Set low and high entry numbers, 1-based. */
			low = 1;
			high = xfs_btree_get_numrecs(block);
			if (!high) {
				/* Block is empty, must be an empty leaf. */
				if (level != 0 || cur->bc_nlevels != 1) {
					XFS_CORRUPTION_ERROR(__func__,
							XFS_ERRLEVEL_LOW,
							cur->bc_mp, block,
							sizeof(*block));
					xfs_btree_mark_sick(cur);
					return -EFSCORRUPTED;
				}

				cur->bc_levels[0].ptr = dir != XFS_LOOKUP_LE;
				*stat = 0;
				return 0;
			}

			/* Binary search the block. */
			while (low <= high) {
				union xfs_btree_key	key;
				union xfs_btree_key	*kp;

				XFS_BTREE_STATS_INC(cur, compare);

				/* keyno is average of low and high. */
				keyno = (low + high) >> 1;

				/* Get current search key */
				kp = xfs_lookup_get_search_key(cur, level,
						keyno, block, &key);

				/*
				 * Compute comparison result to get next
				 * direction:
				 *  - less than, move right
				 *  - greater than, move left
				 *  - equal, we're done
				 */
				cmp_r = cur->bc_ops->cmp_key_with_cur(cur, kp);
				if (cmp_r < 0)
					low = keyno + 1;
				else if (cmp_r > 0)
					high = keyno - 1;
				else
					break;
			}
		}

		/*
		 * If there are more levels, set up for the next level
		 * by getting the block number and filling in the cursor.
		 */
		if (level > 0) {
			/*
			 * If we moved left, need the previous key number,
			 * unless there isn't one.
			 */
			if (cmp_r > 0 && --keyno < 1)
				keyno = 1;
			pp = xfs_btree_ptr_addr(cur, keyno, block);

			error = xfs_btree_debug_check_ptr(cur, pp, 0, level);
			if (error)
				goto error0;

			cur->bc_levels[level].ptr = keyno;
		}
	}

	/* Done with the search. See if we need to adjust the results. */
	if (dir != XFS_LOOKUP_LE && cmp_r < 0) {
		keyno++;
		/*
		 * If ge search and we went off the end of the block, but it's
		 * not the last block, we're in the wrong block.
		 */
		xfs_btree_get_sibling(cur, block, &ptr, XFS_BB_RIGHTSIB);
		if (dir == XFS_LOOKUP_GE &&
		    keyno > xfs_btree_get_numrecs(block) &&
		    !xfs_btree_ptr_is_null(cur, &ptr)) {
			int	i;

			cur->bc_levels[0].ptr = keyno;
			error = xfs_btree_increment(cur, 0, &i);
			if (error)
				goto error0;
			if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				return -EFSCORRUPTED;
			}
			*stat = 1;
			return 0;
		}
	} else if (dir == XFS_LOOKUP_LE && cmp_r > 0)
		keyno--;
	cur->bc_levels[0].ptr = keyno;

	/* Return if we succeeded or not. */
	if (keyno == 0 || keyno > xfs_btree_get_numrecs(block))
		*stat = 0;
	else if (dir != XFS_LOOKUP_EQ || cmp_r == 0)
		*stat = 1;
	else
		*stat = 0;
	return 0;

error0:
	return error;
}

/* Find the high key storage area from a regular key. */
union xfs_btree_key *
xfs_btree_high_key_from_key(
	struct xfs_btree_cur	*cur,
	union xfs_btree_key	*key)
{
	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING);
	return (union xfs_btree_key *)((char *)key +
			(cur->bc_ops->key_len / 2));
}

/* Determine the low (and high if overlapped) keys of a leaf block */
STATIC void
xfs_btree_get_leaf_keys(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	union xfs_btree_key	*key)
{
	union xfs_btree_key	max_hkey;
	union xfs_btree_key	hkey;
	union xfs_btree_rec	*rec;
	union xfs_btree_key	*high;
	int			n;

	rec = xfs_btree_rec_addr(cur, 1, block);
	cur->bc_ops->init_key_from_rec(key, rec);

	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) {

		cur->bc_ops->init_high_key_from_rec(&max_hkey, rec);
		for (n = 2; n <= xfs_btree_get_numrecs(block); n++) {
			rec = xfs_btree_rec_addr(cur, n, block);
			cur->bc_ops->init_high_key_from_rec(&hkey, rec);
			if (xfs_btree_keycmp_gt(cur, &hkey, &max_hkey))
				max_hkey = hkey;
		}

		high = xfs_btree_high_key_from_key(cur, key);
		memcpy(high, &max_hkey, cur->bc_ops->key_len / 2);
	}
}

/* Determine the low (and high if overlapped) keys of a node block */
STATIC void
xfs_btree_get_node_keys(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	union xfs_btree_key	*key)
{
	union xfs_btree_key	*hkey;
	union xfs_btree_key	*max_hkey;
	union xfs_btree_key	*high;
	int			n;

	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) {
		memcpy(key, xfs_btree_key_addr(cur, 1, block),
				cur->bc_ops->key_len / 2);

		max_hkey = xfs_btree_high_key_addr(cur, 1, block);
		for (n = 2; n <= xfs_btree_get_numrecs(block); n++) {
			hkey = xfs_btree_high_key_addr(cur, n, block);
			if (xfs_btree_keycmp_gt(cur, hkey, max_hkey))
				max_hkey = hkey;
		}

		high = xfs_btree_high_key_from_key(cur, key);
		memcpy(high, max_hkey, cur->bc_ops->key_len / 2);
	} else {
		memcpy(key, xfs_btree_key_addr(cur, 1, block),
				cur->bc_ops->key_len);
	}
}

/* Derive the keys for any btree block. */
void
xfs_btree_get_keys(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	union xfs_btree_key	*key)
{
	if (be16_to_cpu(block->bb_level) == 0)
		xfs_btree_get_leaf_keys(cur, block, key);
	else
		xfs_btree_get_node_keys(cur, block, key);
}

/*
 * Decide if we need to update the parent keys of a btree block.  For
 * a standard btree this is only necessary if we're updating the first
 * record/key.  For an overlapping btree, we must always update the
 * keys because the highest key can be in any of the records or keys
 * in the block.
 */
static inline bool
xfs_btree_needs_key_update(
	struct xfs_btree_cur	*cur,
	int			ptr)
{
	return (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) || ptr == 1;
}

/*
 * Update the low and high parent keys of the given level, progressing
 * towards the root.  If force_all is false, stop if the keys for a given
 * level do not need updating.
 */
STATIC int
__xfs_btree_updkeys(
	struct xfs_btree_cur	*cur,
	int			level,
	struct xfs_btree_block	*block,
	struct xfs_buf		*bp0,
	bool			force_all)
{
	union xfs_btree_key	key;	/* keys from current level */
	union xfs_btree_key	*lkey;	/* keys from the next level up */
	union xfs_btree_key	*hkey;
	union xfs_btree_key	*nlkey;	/* keys from the next level up */
	union xfs_btree_key	*nhkey;
	struct xfs_buf		*bp;
	int			ptr;

	ASSERT(cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING);

	/* Exit if there aren't any parent levels to update. */
	if (level + 1 >= cur->bc_nlevels)
		return 0;

	trace_xfs_btree_updkeys(cur, level, bp0);

	lkey = &key;
	hkey = xfs_btree_high_key_from_key(cur, lkey);
	xfs_btree_get_keys(cur, block, lkey);
	for (level++; level < cur->bc_nlevels; level++) {
#ifdef DEBUG
		int		error;
#endif
		block = xfs_btree_get_block(cur, level, &bp);
		trace_xfs_btree_updkeys(cur, level, bp);
#ifdef DEBUG
		error = xfs_btree_check_block(cur, block, level, bp);
		if (error)
			return error;
#endif
		ptr = cur->bc_levels[level].ptr;
		nlkey = xfs_btree_key_addr(cur, ptr, block);
		nhkey = xfs_btree_high_key_addr(cur, ptr, block);
		if (!force_all &&
		    xfs_btree_keycmp_eq(cur, nlkey, lkey) &&
		    xfs_btree_keycmp_eq(cur, nhkey, hkey))
			break;
		xfs_btree_copy_keys(cur, nlkey, lkey, 1);
		xfs_btree_log_keys(cur, bp, ptr, ptr);
		if (level + 1 >= cur->bc_nlevels)
			break;
		xfs_btree_get_node_keys(cur, block, lkey);
	}

	return 0;
}

/* Update all the keys from some level in cursor back to the root. */
STATIC int
xfs_btree_updkeys_force(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfs_buf		*bp;
	struct xfs_btree_block	*block;

	block = xfs_btree_get_block(cur, level, &bp);
	return __xfs_btree_updkeys(cur, level, block, bp, true);
}

/*
 * Update the parent keys of the given level, progressing towards the root.
 */
STATIC int
xfs_btree_update_keys(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;
	union xfs_btree_key	*kp;
	union xfs_btree_key	key;
	int			ptr;

	ASSERT(level >= 0);

	block = xfs_btree_get_block(cur, level, &bp);
	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING)
		return __xfs_btree_updkeys(cur, level, block, bp, false);

	/*
	 * Go up the tree from this level toward the root.
	 * At each level, update the key value to the value input.
	 * Stop when we reach a level where the cursor isn't pointing
	 * at the first entry in the block.
	 */
	xfs_btree_get_keys(cur, block, &key);
	for (level++, ptr = 1; ptr == 1 && level < cur->bc_nlevels; level++) {
#ifdef DEBUG
		int		error;
#endif
		block = xfs_btree_get_block(cur, level, &bp);
#ifdef DEBUG
		error = xfs_btree_check_block(cur, block, level, bp);
		if (error)
			return error;
#endif
		ptr = cur->bc_levels[level].ptr;
		kp = xfs_btree_key_addr(cur, ptr, block);
		xfs_btree_copy_keys(cur, kp, &key, 1);
		xfs_btree_log_keys(cur, bp, ptr, ptr);
	}

	return 0;
}

/*
 * Update the record referred to by cur to the value in the
 * given record. This either works (return 0) or gets an
 * EFSCORRUPTED error.
 */
int
xfs_btree_update(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec)
{
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;
	int			error;
	int			ptr;
	union xfs_btree_rec	*rp;

	/* Pick up the current block. */
	block = xfs_btree_get_block(cur, 0, &bp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, 0, bp);
	if (error)
		goto error0;
#endif
	/* Get the address of the rec to be updated. */
	ptr = cur->bc_levels[0].ptr;
	rp = xfs_btree_rec_addr(cur, ptr, block);

	/* Fill in the new contents and log them. */
	xfs_btree_copy_recs(cur, rp, rec, 1);
	xfs_btree_log_recs(cur, bp, ptr, ptr);

	/* Pass new key value up to our parent. */
	if (xfs_btree_needs_key_update(cur, ptr)) {
		error = xfs_btree_update_keys(cur, 0);
		if (error)
			goto error0;
	}

	return 0;

error0:
	return error;
}

/*
 * Move 1 record left from cur/level if possible.
 * Update cur to reflect the new path.
 */
STATIC int					/* error */
xfs_btree_lshift(
	struct xfs_btree_cur	*cur,
	int			level,
	int			*stat)		/* success/failure */
{
	struct xfs_buf		*lbp;		/* left buffer pointer */
	struct xfs_btree_block	*left;		/* left btree block */
	int			lrecs;		/* left record count */
	struct xfs_buf		*rbp;		/* right buffer pointer */
	struct xfs_btree_block	*right;		/* right btree block */
	struct xfs_btree_cur	*tcur;		/* temporary btree cursor */
	int			rrecs;		/* right record count */
	union xfs_btree_ptr	lptr;		/* left btree pointer */
	union xfs_btree_key	*rkp = NULL;	/* right btree key */
	union xfs_btree_ptr	*rpp = NULL;	/* right address pointer */
	union xfs_btree_rec	*rrp = NULL;	/* right record pointer */
	int			error;		/* error return value */
	int			i;

	if (xfs_btree_at_iroot(cur, level))
		goto out0;

	/* Set up variables for this block as "right". */
	right = xfs_btree_get_block(cur, level, &rbp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, right, level, rbp);
	if (error)
		goto error0;
#endif

	/* If we've got no left sibling then we can't shift an entry left. */
	xfs_btree_get_sibling(cur, right, &lptr, XFS_BB_LEFTSIB);
	if (xfs_btree_ptr_is_null(cur, &lptr))
		goto out0;

	/*
	 * If the cursor entry is the one that would be moved, don't
	 * do it... it's too complicated.
	 */
	if (cur->bc_levels[level].ptr <= 1)
		goto out0;

	/* Set up the left neighbor as "left". */
	error = xfs_btree_read_buf_block(cur, &lptr, 0, &left, &lbp);
	if (error)
		goto error0;

	/* If it's full, it can't take another entry. */
	lrecs = xfs_btree_get_numrecs(left);
	if (lrecs == cur->bc_ops->get_maxrecs(cur, level))
		goto out0;

	rrecs = xfs_btree_get_numrecs(right);

	/*
	 * We add one entry to the left side and remove one for the right side.
	 * Account for it here, the changes will be updated on disk and logged
	 * later.
	 */
	lrecs++;
	rrecs--;

	XFS_BTREE_STATS_INC(cur, lshift);
	XFS_BTREE_STATS_ADD(cur, moves, 1);

	/*
	 * If non-leaf, copy a key and a ptr to the left block.
	 * Log the changes to the left block.
	 */
	if (level > 0) {
		/* It's a non-leaf.  Move keys and pointers. */
		union xfs_btree_key	*lkp;	/* left btree key */
		union xfs_btree_ptr	*lpp;	/* left address pointer */

		lkp = xfs_btree_key_addr(cur, lrecs, left);
		rkp = xfs_btree_key_addr(cur, 1, right);

		lpp = xfs_btree_ptr_addr(cur, lrecs, left);
		rpp = xfs_btree_ptr_addr(cur, 1, right);

		error = xfs_btree_debug_check_ptr(cur, rpp, 0, level);
		if (error)
			goto error0;

		xfs_btree_copy_keys(cur, lkp, rkp, 1);
		xfs_btree_copy_ptrs(cur, lpp, rpp, 1);

		xfs_btree_log_keys(cur, lbp, lrecs, lrecs);
		xfs_btree_log_ptrs(cur, lbp, lrecs, lrecs);

		ASSERT(cur->bc_ops->keys_inorder(cur,
			xfs_btree_key_addr(cur, lrecs - 1, left), lkp));
	} else {
		/* It's a leaf.  Move records.  */
		union xfs_btree_rec	*lrp;	/* left record pointer */

		lrp = xfs_btree_rec_addr(cur, lrecs, left);
		rrp = xfs_btree_rec_addr(cur, 1, right);

		xfs_btree_copy_recs(cur, lrp, rrp, 1);
		xfs_btree_log_recs(cur, lbp, lrecs, lrecs);

		ASSERT(cur->bc_ops->recs_inorder(cur,
			xfs_btree_rec_addr(cur, lrecs - 1, left), lrp));
	}

	xfs_btree_set_numrecs(left, lrecs);
	xfs_btree_log_block(cur, lbp, XFS_BB_NUMRECS);

	xfs_btree_set_numrecs(right, rrecs);
	xfs_btree_log_block(cur, rbp, XFS_BB_NUMRECS);

	/*
	 * Slide the contents of right down one entry.
	 */
	XFS_BTREE_STATS_ADD(cur, moves, rrecs - 1);
	if (level > 0) {
		/* It's a nonleaf. operate on keys and ptrs */
		for (i = 0; i < rrecs; i++) {
			error = xfs_btree_debug_check_ptr(cur, rpp, i + 1, level);
			if (error)
				goto error0;
		}

		xfs_btree_shift_keys(cur,
				xfs_btree_key_addr(cur, 2, right),
				-1, rrecs);
		xfs_btree_shift_ptrs(cur,
				xfs_btree_ptr_addr(cur, 2, right),
				-1, rrecs);

		xfs_btree_log_keys(cur, rbp, 1, rrecs);
		xfs_btree_log_ptrs(cur, rbp, 1, rrecs);
	} else {
		/* It's a leaf. operate on records */
		xfs_btree_shift_recs(cur,
			xfs_btree_rec_addr(cur, 2, right),
			-1, rrecs);
		xfs_btree_log_recs(cur, rbp, 1, rrecs);
	}

	/*
	 * Using a temporary cursor, update the parent key values of the
	 * block on the left.
	 */
	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) {
		error = xfs_btree_dup_cursor(cur, &tcur);
		if (error)
			goto error0;
		i = xfs_btree_firstrec(tcur, level);
		if (XFS_IS_CORRUPT(tcur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		error = xfs_btree_decrement(tcur, level, &i);
		if (error)
			goto error1;

		/* Update the parent high keys of the left block, if needed. */
		error = xfs_btree_update_keys(tcur, level);
		if (error)
			goto error1;

		xfs_btree_del_cursor(tcur, XFS_BTREE_NOERROR);
	}

	/* Update the parent keys of the right block. */
	error = xfs_btree_update_keys(cur, level);
	if (error)
		goto error0;

	/* Slide the cursor value left one. */
	cur->bc_levels[level].ptr--;

	*stat = 1;
	return 0;

out0:
	*stat = 0;
	return 0;

error0:
	return error;

error1:
	xfs_btree_del_cursor(tcur, XFS_BTREE_ERROR);
	return error;
}

/*
 * Move 1 record right from cur/level if possible.
 * Update cur to reflect the new path.
 */
STATIC int					/* error */
xfs_btree_rshift(
	struct xfs_btree_cur	*cur,
	int			level,
	int			*stat)		/* success/failure */
{
	struct xfs_buf		*lbp;		/* left buffer pointer */
	struct xfs_btree_block	*left;		/* left btree block */
	struct xfs_buf		*rbp;		/* right buffer pointer */
	struct xfs_btree_block	*right;		/* right btree block */
	struct xfs_btree_cur	*tcur;		/* temporary btree cursor */
	union xfs_btree_ptr	rptr;		/* right block pointer */
	union xfs_btree_key	*rkp;		/* right btree key */
	int			rrecs;		/* right record count */
	int			lrecs;		/* left record count */
	int			error;		/* error return value */
	int			i;		/* loop counter */

	if (xfs_btree_at_iroot(cur, level))
		goto out0;

	/* Set up variables for this block as "left". */
	left = xfs_btree_get_block(cur, level, &lbp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, left, level, lbp);
	if (error)
		goto error0;
#endif

	/* If we've got no right sibling then we can't shift an entry right. */
	xfs_btree_get_sibling(cur, left, &rptr, XFS_BB_RIGHTSIB);
	if (xfs_btree_ptr_is_null(cur, &rptr))
		goto out0;

	/*
	 * If the cursor entry is the one that would be moved, don't
	 * do it... it's too complicated.
	 */
	lrecs = xfs_btree_get_numrecs(left);
	if (cur->bc_levels[level].ptr >= lrecs)
		goto out0;

	/* Set up the right neighbor as "right". */
	error = xfs_btree_read_buf_block(cur, &rptr, 0, &right, &rbp);
	if (error)
		goto error0;

	/* If it's full, it can't take another entry. */
	rrecs = xfs_btree_get_numrecs(right);
	if (rrecs == cur->bc_ops->get_maxrecs(cur, level))
		goto out0;

	XFS_BTREE_STATS_INC(cur, rshift);
	XFS_BTREE_STATS_ADD(cur, moves, rrecs);

	/*
	 * Make a hole at the start of the right neighbor block, then
	 * copy the last left block entry to the hole.
	 */
	if (level > 0) {
		/* It's a nonleaf. make a hole in the keys and ptrs */
		union xfs_btree_key	*lkp;
		union xfs_btree_ptr	*lpp;
		union xfs_btree_ptr	*rpp;

		lkp = xfs_btree_key_addr(cur, lrecs, left);
		lpp = xfs_btree_ptr_addr(cur, lrecs, left);
		rkp = xfs_btree_key_addr(cur, 1, right);
		rpp = xfs_btree_ptr_addr(cur, 1, right);

		for (i = rrecs - 1; i >= 0; i--) {
			error = xfs_btree_debug_check_ptr(cur, rpp, i, level);
			if (error)
				goto error0;
		}

		xfs_btree_shift_keys(cur, rkp, 1, rrecs);
		xfs_btree_shift_ptrs(cur, rpp, 1, rrecs);

		error = xfs_btree_debug_check_ptr(cur, lpp, 0, level);
		if (error)
			goto error0;

		/* Now put the new data in, and log it. */
		xfs_btree_copy_keys(cur, rkp, lkp, 1);
		xfs_btree_copy_ptrs(cur, rpp, lpp, 1);

		xfs_btree_log_keys(cur, rbp, 1, rrecs + 1);
		xfs_btree_log_ptrs(cur, rbp, 1, rrecs + 1);

		ASSERT(cur->bc_ops->keys_inorder(cur, rkp,
			xfs_btree_key_addr(cur, 2, right)));
	} else {
		/* It's a leaf. make a hole in the records */
		union xfs_btree_rec	*lrp;
		union xfs_btree_rec	*rrp;

		lrp = xfs_btree_rec_addr(cur, lrecs, left);
		rrp = xfs_btree_rec_addr(cur, 1, right);

		xfs_btree_shift_recs(cur, rrp, 1, rrecs);

		/* Now put the new data in, and log it. */
		xfs_btree_copy_recs(cur, rrp, lrp, 1);
		xfs_btree_log_recs(cur, rbp, 1, rrecs + 1);
	}

	/*
	 * Decrement and log left's numrecs, bump and log right's numrecs.
	 */
	xfs_btree_set_numrecs(left, --lrecs);
	xfs_btree_log_block(cur, lbp, XFS_BB_NUMRECS);

	xfs_btree_set_numrecs(right, ++rrecs);
	xfs_btree_log_block(cur, rbp, XFS_BB_NUMRECS);

	/*
	 * Using a temporary cursor, update the parent key values of the
	 * block on the right.
	 */
	error = xfs_btree_dup_cursor(cur, &tcur);
	if (error)
		goto error0;
	i = xfs_btree_lastrec(tcur, level);
	if (XFS_IS_CORRUPT(tcur->bc_mp, i != 1)) {
		xfs_btree_mark_sick(cur);
		error = -EFSCORRUPTED;
		goto error0;
	}

	error = xfs_btree_increment(tcur, level, &i);
	if (error)
		goto error1;

	/* Update the parent high keys of the left block, if needed. */
	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) {
		error = xfs_btree_update_keys(cur, level);
		if (error)
			goto error1;
	}

	/* Update the parent keys of the right block. */
	error = xfs_btree_update_keys(tcur, level);
	if (error)
		goto error1;

	xfs_btree_del_cursor(tcur, XFS_BTREE_NOERROR);

	*stat = 1;
	return 0;

out0:
	*stat = 0;
	return 0;

error0:
	return error;

error1:
	xfs_btree_del_cursor(tcur, XFS_BTREE_ERROR);
	return error;
}

static inline int
xfs_btree_alloc_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*hint_block,
	union xfs_btree_ptr		*new_block,
	int				*stat)
{
	int				error;

	/*
	 * Don't allow block allocation for a staging cursor, because staging
	 * cursors do not support regular btree modifications.
	 *
	 * Bulk loading uses a separate callback to obtain new blocks from a
	 * preallocated list, which prevents ENOSPC failures during loading.
	 */
	if (unlikely(cur->bc_flags & XFS_BTREE_STAGING)) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	error = cur->bc_ops->alloc_block(cur, hint_block, new_block, stat);
	trace_xfs_btree_alloc_block(cur, new_block, *stat, error);
	return error;
}

/*
 * Split cur/level block in half.
 * Return new block number and the key to its first
 * record (to be inserted into parent).
 */
STATIC int					/* error */
__xfs_btree_split(
	struct xfs_btree_cur	*cur,
	int			level,
	union xfs_btree_ptr	*ptrp,
	union xfs_btree_key	*key,
	struct xfs_btree_cur	**curp,
	int			*stat)		/* success/failure */
{
	union xfs_btree_ptr	lptr;		/* left sibling block ptr */
	struct xfs_buf		*lbp;		/* left buffer pointer */
	struct xfs_btree_block	*left;		/* left btree block */
	union xfs_btree_ptr	rptr;		/* right sibling block ptr */
	struct xfs_buf		*rbp;		/* right buffer pointer */
	struct xfs_btree_block	*right;		/* right btree block */
	union xfs_btree_ptr	rrptr;		/* right-right sibling ptr */
	struct xfs_buf		*rrbp;		/* right-right buffer pointer */
	struct xfs_btree_block	*rrblock;	/* right-right btree block */
	int			lrecs;
	int			rrecs;
	int			src_index;
	int			error;		/* error return value */
	int			i;

	XFS_BTREE_STATS_INC(cur, split);

	/* Set up left block (current one). */
	left = xfs_btree_get_block(cur, level, &lbp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, left, level, lbp);
	if (error)
		goto error0;
#endif

	xfs_btree_buf_to_ptr(cur, lbp, &lptr);

	/* Allocate the new block. If we can't do it, we're toast. Give up. */
	error = xfs_btree_alloc_block(cur, &lptr, &rptr, stat);
	if (error)
		goto error0;
	if (*stat == 0)
		goto out0;
	XFS_BTREE_STATS_INC(cur, alloc);

	/* Set up the new block as "right". */
	error = xfs_btree_get_buf_block(cur, &rptr, &right, &rbp);
	if (error)
		goto error0;

	/* Fill in the btree header for the new right block. */
	xfs_btree_init_block_cur(cur, rbp, xfs_btree_get_level(left), 0);

	/*
	 * Split the entries between the old and the new block evenly.
	 * Make sure that if there's an odd number of entries now, that
	 * each new block will have the same number of entries.
	 */
	lrecs = xfs_btree_get_numrecs(left);
	rrecs = lrecs / 2;
	if ((lrecs & 1) && cur->bc_levels[level].ptr <= rrecs + 1)
		rrecs++;
	src_index = (lrecs - rrecs + 1);

	XFS_BTREE_STATS_ADD(cur, moves, rrecs);

	/* Adjust numrecs for the later get_*_keys() calls. */
	lrecs -= rrecs;
	xfs_btree_set_numrecs(left, lrecs);
	xfs_btree_set_numrecs(right, xfs_btree_get_numrecs(right) + rrecs);

	/*
	 * Copy btree block entries from the left block over to the
	 * new block, the right. Update the right block and log the
	 * changes.
	 */
	if (level > 0) {
		/* It's a non-leaf.  Move keys and pointers. */
		union xfs_btree_key	*lkp;	/* left btree key */
		union xfs_btree_ptr	*lpp;	/* left address pointer */
		union xfs_btree_key	*rkp;	/* right btree key */
		union xfs_btree_ptr	*rpp;	/* right address pointer */

		lkp = xfs_btree_key_addr(cur, src_index, left);
		lpp = xfs_btree_ptr_addr(cur, src_index, left);
		rkp = xfs_btree_key_addr(cur, 1, right);
		rpp = xfs_btree_ptr_addr(cur, 1, right);

		for (i = src_index; i < rrecs; i++) {
			error = xfs_btree_debug_check_ptr(cur, lpp, i, level);
			if (error)
				goto error0;
		}

		/* Copy the keys & pointers to the new block. */
		xfs_btree_copy_keys(cur, rkp, lkp, rrecs);
		xfs_btree_copy_ptrs(cur, rpp, lpp, rrecs);

		xfs_btree_log_keys(cur, rbp, 1, rrecs);
		xfs_btree_log_ptrs(cur, rbp, 1, rrecs);

		/* Stash the keys of the new block for later insertion. */
		xfs_btree_get_node_keys(cur, right, key);
	} else {
		/* It's a leaf.  Move records.  */
		union xfs_btree_rec	*lrp;	/* left record pointer */
		union xfs_btree_rec	*rrp;	/* right record pointer */

		lrp = xfs_btree_rec_addr(cur, src_index, left);
		rrp = xfs_btree_rec_addr(cur, 1, right);

		/* Copy records to the new block. */
		xfs_btree_copy_recs(cur, rrp, lrp, rrecs);
		xfs_btree_log_recs(cur, rbp, 1, rrecs);

		/* Stash the keys of the new block for later insertion. */
		xfs_btree_get_leaf_keys(cur, right, key);
	}

	/*
	 * Find the left block number by looking in the buffer.
	 * Adjust sibling pointers.
	 */
	xfs_btree_get_sibling(cur, left, &rrptr, XFS_BB_RIGHTSIB);
	xfs_btree_set_sibling(cur, right, &rrptr, XFS_BB_RIGHTSIB);
	xfs_btree_set_sibling(cur, right, &lptr, XFS_BB_LEFTSIB);
	xfs_btree_set_sibling(cur, left, &rptr, XFS_BB_RIGHTSIB);

	xfs_btree_log_block(cur, rbp, XFS_BB_ALL_BITS);
	xfs_btree_log_block(cur, lbp, XFS_BB_NUMRECS | XFS_BB_RIGHTSIB);

	/*
	 * If there's a block to the new block's right, make that block
	 * point back to right instead of to left.
	 */
	if (!xfs_btree_ptr_is_null(cur, &rrptr)) {
		error = xfs_btree_read_buf_block(cur, &rrptr,
							0, &rrblock, &rrbp);
		if (error)
			goto error0;
		xfs_btree_set_sibling(cur, rrblock, &rptr, XFS_BB_LEFTSIB);
		xfs_btree_log_block(cur, rrbp, XFS_BB_LEFTSIB);
	}

	/* Update the parent high keys of the left block, if needed. */
	if (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING) {
		error = xfs_btree_update_keys(cur, level);
		if (error)
			goto error0;
	}

	/*
	 * If the cursor is really in the right block, move it there.
	 * If it's just pointing past the last entry in left, then we'll
	 * insert there, so don't change anything in that case.
	 */
	if (cur->bc_levels[level].ptr > lrecs + 1) {
		xfs_btree_setbuf(cur, level, rbp);
		cur->bc_levels[level].ptr -= lrecs;
	}
	/*
	 * If there are more levels, we'll need another cursor which refers
	 * the right block, no matter where this cursor was.
	 */
	if (level + 1 < cur->bc_nlevels) {
		error = xfs_btree_dup_cursor(cur, curp);
		if (error)
			goto error0;
		(*curp)->bc_levels[level + 1].ptr++;
	}
	*ptrp = rptr;
	*stat = 1;
	return 0;
out0:
	*stat = 0;
	return 0;

error0:
	return error;
}

#ifdef __KERNEL__
struct xfs_btree_split_args {
	struct xfs_btree_cur	*cur;
	int			level;
	union xfs_btree_ptr	*ptrp;
	union xfs_btree_key	*key;
	struct xfs_btree_cur	**curp;
	int			*stat;		/* success/failure */
	int			result;
	bool			kswapd;	/* allocation in kswapd context */
	struct completion	*done;
	struct work_struct	work;
};

/*
 * Stack switching interfaces for allocation
 */
static void
xfs_btree_split_worker(
	struct work_struct	*work)
{
	struct xfs_btree_split_args	*args = container_of(work,
						struct xfs_btree_split_args, work);
	unsigned long		pflags;
	unsigned long		new_pflags = 0;

	/*
	 * we are in a transaction context here, but may also be doing work
	 * in kswapd context, and hence we may need to inherit that state
	 * temporarily to ensure that we don't block waiting for memory reclaim
	 * in any way.
	 */
	if (args->kswapd)
		new_pflags |= PF_MEMALLOC | PF_KSWAPD;

	current_set_flags_nested(&pflags, new_pflags);
	xfs_trans_set_context(args->cur->bc_tp);

	args->result = __xfs_btree_split(args->cur, args->level, args->ptrp,
					 args->key, args->curp, args->stat);

	xfs_trans_clear_context(args->cur->bc_tp);
	current_restore_flags_nested(&pflags, new_pflags);

	/*
	 * Do not access args after complete() has run here. We don't own args
	 * and the owner may run and free args before we return here.
	 */
	complete(args->done);

}

/*
 * BMBT split requests often come in with little stack to work on so we push
 * them off to a worker thread so there is lots of stack to use. For the other
 * btree types, just call directly to avoid the context switch overhead here.
 *
 * Care must be taken here - the work queue rescuer thread introduces potential
 * AGF <> worker queue deadlocks if the BMBT block allocation has to lock new
 * AGFs to allocate blocks. A task being run by the rescuer could attempt to
 * lock an AGF that is already locked by a task queued to run by the rescuer,
 * resulting in an ABBA deadlock as the rescuer cannot run the lock holder to
 * release it until the current thread it is running gains the lock.
 *
 * To avoid this issue, we only ever queue BMBT splits that don't have an AGF
 * already locked to allocate from. The only place that doesn't hold an AGF
 * locked is unwritten extent conversion at IO completion, but that has already
 * been offloaded to a worker thread and hence has no stack consumption issues
 * we have to worry about.
 */
STATIC int					/* error */
xfs_btree_split(
	struct xfs_btree_cur	*cur,
	int			level,
	union xfs_btree_ptr	*ptrp,
	union xfs_btree_key	*key,
	struct xfs_btree_cur	**curp,
	int			*stat)		/* success/failure */
{
	struct xfs_btree_split_args	args;
	DECLARE_COMPLETION_ONSTACK(done);

	if (!xfs_btree_is_bmap(cur->bc_ops) ||
	    cur->bc_tp->t_highest_agno == NULLAGNUMBER)
		return __xfs_btree_split(cur, level, ptrp, key, curp, stat);

	args.cur = cur;
	args.level = level;
	args.ptrp = ptrp;
	args.key = key;
	args.curp = curp;
	args.stat = stat;
	args.done = &done;
	args.kswapd = current_is_kswapd();
	INIT_WORK_ONSTACK(&args.work, xfs_btree_split_worker);
	queue_work(xfs_alloc_wq, &args.work);
	wait_for_completion(&done);
	destroy_work_on_stack(&args.work);
	return args.result;
}
#else
#define xfs_btree_split	__xfs_btree_split
#endif /* __KERNEL__ */

/* Move the records from a root leaf block to a separate block. */
STATIC void
xfs_btree_promote_leaf_iroot(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	struct xfs_buf		*cbp,
	union xfs_btree_ptr	*cptr,
	struct xfs_btree_block	*cblock)
{
	union xfs_btree_rec	*rp;
	union xfs_btree_rec	*crp;
	union xfs_btree_key	*kp;
	union xfs_btree_ptr	*pp;
	struct xfs_btree_block	*broot;
	int			numrecs = xfs_btree_get_numrecs(block);

	/* Copy the records from the leaf broot into the new child block. */
	rp = xfs_btree_rec_addr(cur, 1, block);
	crp = xfs_btree_rec_addr(cur, 1, cblock);
	xfs_btree_copy_recs(cur, crp, rp, numrecs);

	/*
	 * Increment the tree height.
	 *
	 * Trickery here: The amount of memory that we need per record for the
	 * ifork's btree root block may change when we convert the broot from a
	 * leaf to a node block.  Free the existing leaf broot so that nobody
	 * thinks we need to migrate node pointers when we realloc the broot
	 * buffer after bumping nlevels.
	 */
	cur->bc_ops->broot_realloc(cur, 0);
	cur->bc_nlevels++;
	cur->bc_levels[1].ptr = 1;

	/*
	 * Allocate a new node broot and initialize it to point to the new
	 * child block.
	 */
	broot = cur->bc_ops->broot_realloc(cur, 1);
	xfs_btree_init_block(cur->bc_mp, broot, cur->bc_ops,
			cur->bc_nlevels - 1, 1, cur->bc_ino.ip->i_ino);

	pp = xfs_btree_ptr_addr(cur, 1, broot);
	kp = xfs_btree_key_addr(cur, 1, broot);
	xfs_btree_copy_ptrs(cur, pp, cptr, 1);
	xfs_btree_get_keys(cur, cblock, kp);

	/* Attach the new block to the cursor and log it. */
	xfs_btree_setbuf(cur, 0, cbp);
	xfs_btree_log_block(cur, cbp, XFS_BB_ALL_BITS);
	xfs_btree_log_recs(cur, cbp, 1, numrecs);
}

/*
 * Move the keys and pointers from a root block to a separate block.
 *
 * Since the keyptr size does not change, all we have to do is increase the
 * tree height, copy the keyptrs to the new internal node (cblock), shrink
 * the root, and copy the pointers there.
 */
STATIC int
xfs_btree_promote_node_iroot(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*block,
	int			level,
	struct xfs_buf		*cbp,
	union xfs_btree_ptr	*cptr,
	struct xfs_btree_block	*cblock)
{
	union xfs_btree_key	*ckp;
	union xfs_btree_key	*kp;
	union xfs_btree_ptr	*cpp;
	union xfs_btree_ptr	*pp;
	int			i;
	int			error;
	int			numrecs = xfs_btree_get_numrecs(block);

	/*
	 * Increase tree height, adjusting the root block level to match.
	 * We cannot change the root btree node size until we've copied the
	 * block contents to the new child block.
	 */
	be16_add_cpu(&block->bb_level, 1);
	cur->bc_nlevels++;
	cur->bc_levels[level + 1].ptr = 1;

	/*
	 * Adjust the root btree record count, then copy the keys from the old
	 * root to the new child block.
	 */
	xfs_btree_set_numrecs(block, 1);
	kp = xfs_btree_key_addr(cur, 1, block);
	ckp = xfs_btree_key_addr(cur, 1, cblock);
	xfs_btree_copy_keys(cur, ckp, kp, numrecs);

	/* Check the pointers and copy them to the new child block. */
	pp = xfs_btree_ptr_addr(cur, 1, block);
	cpp = xfs_btree_ptr_addr(cur, 1, cblock);
	for (i = 0; i < numrecs; i++) {
		error = xfs_btree_debug_check_ptr(cur, pp, i, level);
		if (error)
			return error;
	}
	xfs_btree_copy_ptrs(cur, cpp, pp, numrecs);

	/*
	 * Set the first keyptr to point to the new child block, then shrink
	 * the memory buffer for the root block.
	 */
	error = xfs_btree_debug_check_ptr(cur, cptr, 0, level);
	if (error)
		return error;
	xfs_btree_copy_ptrs(cur, pp, cptr, 1);
	xfs_btree_get_keys(cur, cblock, kp);

	cur->bc_ops->broot_realloc(cur, 1);

	/* Attach the new block to the cursor and log it. */
	xfs_btree_setbuf(cur, level, cbp);
	xfs_btree_log_block(cur, cbp, XFS_BB_ALL_BITS);
	xfs_btree_log_keys(cur, cbp, 1, numrecs);
	xfs_btree_log_ptrs(cur, cbp, 1, numrecs);
	return 0;
}

/*
 * Copy the old inode root contents into a real block and make the
 * broot point to it.
 */
int						/* error */
xfs_btree_new_iroot(
	struct xfs_btree_cur	*cur,		/* btree cursor */
	int			*logflags,	/* logging flags for inode */
	int			*stat)		/* return status - 0 fail */
{
	struct xfs_buf		*cbp;		/* buffer for cblock */
	struct xfs_btree_block	*block;		/* btree block */
	struct xfs_btree_block	*cblock;	/* child btree block */
	union xfs_btree_ptr	aptr;
	union xfs_btree_ptr	nptr;		/* new block addr */
	int			level;		/* btree level */
	int			error;		/* error return code */

	XFS_BTREE_STATS_INC(cur, newroot);

	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_INODE);

	level = cur->bc_nlevels - 1;

	block = xfs_btree_get_iroot(cur);
	ASSERT(level > 0 || (cur->bc_ops->geom_flags & XFS_BTGEO_IROOT_RECORDS));
	if (level > 0)
		aptr = *xfs_btree_ptr_addr(cur, 1, block);
	else
		aptr.l = cpu_to_be64(XFS_INO_TO_FSB(cur->bc_mp,
				cur->bc_ino.ip->i_ino));

	/* Allocate the new block. If we can't do it, we're toast. Give up. */
	error = xfs_btree_alloc_block(cur, &aptr, &nptr, stat);
	if (error)
		goto error0;
	if (*stat == 0)
		return 0;

	XFS_BTREE_STATS_INC(cur, alloc);

	/* Copy the root into a real block. */
	error = xfs_btree_get_buf_block(cur, &nptr, &cblock, &cbp);
	if (error)
		goto error0;

	/*
	 * we can't just memcpy() the root in for CRC enabled btree blocks.
	 * In that case have to also ensure the blkno remains correct
	 */
	memcpy(cblock, block, xfs_btree_block_len(cur));
	if (xfs_has_crc(cur->bc_mp)) {
		__be64 bno = cpu_to_be64(xfs_buf_daddr(cbp));
		if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
			cblock->bb_u.l.bb_blkno = bno;
		else
			cblock->bb_u.s.bb_blkno = bno;
	}

	if (level > 0) {
		error = xfs_btree_promote_node_iroot(cur, block, level, cbp,
				&nptr, cblock);
		if (error)
			goto error0;
	} else {
		xfs_btree_promote_leaf_iroot(cur, block, cbp, &nptr, cblock);
	}

	*logflags |= XFS_ILOG_CORE | xfs_ilog_fbroot(cur->bc_ino.whichfork);
	*stat = 1;
	return 0;
error0:
	return error;
}

static void
xfs_btree_set_root(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*ptr,
	int				inc)
{
	if (cur->bc_flags & XFS_BTREE_STAGING) {
		/* Update the btree root information for a per-AG fake root. */
		cur->bc_ag.afake->af_root = be32_to_cpu(ptr->s);
		cur->bc_ag.afake->af_levels += inc;
	} else {
		cur->bc_ops->set_root(cur, ptr, inc);
	}
}

/*
 * Allocate a new root block, fill it in.
 */
STATIC int				/* error */
xfs_btree_new_root(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			*stat)	/* success/failure */
{
	struct xfs_btree_block	*block;	/* one half of the old root block */
	struct xfs_buf		*bp;	/* buffer containing block */
	int			error;	/* error return value */
	struct xfs_buf		*lbp;	/* left buffer pointer */
	struct xfs_btree_block	*left;	/* left btree block */
	struct xfs_buf		*nbp;	/* new (root) buffer */
	struct xfs_btree_block	*new;	/* new (root) btree block */
	int			nptr;	/* new value for key index, 1 or 2 */
	struct xfs_buf		*rbp;	/* right buffer pointer */
	struct xfs_btree_block	*right;	/* right btree block */
	union xfs_btree_ptr	rptr;
	union xfs_btree_ptr	lptr;

	XFS_BTREE_STATS_INC(cur, newroot);

	/* initialise our start point from the cursor */
	xfs_btree_init_ptr_from_cur(cur, &rptr);

	/* Allocate the new block. If we can't do it, we're toast. Give up. */
	error = xfs_btree_alloc_block(cur, &rptr, &lptr, stat);
	if (error)
		goto error0;
	if (*stat == 0)
		goto out0;
	XFS_BTREE_STATS_INC(cur, alloc);

	/* Set up the new block. */
	error = xfs_btree_get_buf_block(cur, &lptr, &new, &nbp);
	if (error)
		goto error0;

	/* Set the root in the holding structure  increasing the level by 1. */
	xfs_btree_set_root(cur, &lptr, 1);

	/*
	 * At the previous root level there are now two blocks: the old root,
	 * and the new block generated when it was split.  We don't know which
	 * one the cursor is pointing at, so we set up variables "left" and
	 * "right" for each case.
	 */
	block = xfs_btree_get_block(cur, cur->bc_nlevels - 1, &bp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, cur->bc_nlevels - 1, bp);
	if (error)
		goto error0;
#endif

	xfs_btree_get_sibling(cur, block, &rptr, XFS_BB_RIGHTSIB);
	if (!xfs_btree_ptr_is_null(cur, &rptr)) {
		/* Our block is left, pick up the right block. */
		lbp = bp;
		xfs_btree_buf_to_ptr(cur, lbp, &lptr);
		left = block;
		error = xfs_btree_read_buf_block(cur, &rptr, 0, &right, &rbp);
		if (error)
			goto error0;
		bp = rbp;
		nptr = 1;
	} else {
		/* Our block is right, pick up the left block. */
		rbp = bp;
		xfs_btree_buf_to_ptr(cur, rbp, &rptr);
		right = block;
		xfs_btree_get_sibling(cur, right, &lptr, XFS_BB_LEFTSIB);
		error = xfs_btree_read_buf_block(cur, &lptr, 0, &left, &lbp);
		if (error)
			goto error0;
		bp = lbp;
		nptr = 2;
	}

	/* Fill in the new block's btree header and log it. */
	xfs_btree_init_block_cur(cur, nbp, cur->bc_nlevels, 2);
	xfs_btree_log_block(cur, nbp, XFS_BB_ALL_BITS);
	ASSERT(!xfs_btree_ptr_is_null(cur, &lptr) &&
			!xfs_btree_ptr_is_null(cur, &rptr));

	/* Fill in the key data in the new root. */
	if (xfs_btree_get_level(left) > 0) {
		/*
		 * Get the keys for the left block's keys and put them directly
		 * in the parent block.  Do the same for the right block.
		 */
		xfs_btree_get_node_keys(cur, left,
				xfs_btree_key_addr(cur, 1, new));
		xfs_btree_get_node_keys(cur, right,
				xfs_btree_key_addr(cur, 2, new));
	} else {
		/*
		 * Get the keys for the left block's records and put them
		 * directly in the parent block.  Do the same for the right
		 * block.
		 */
		xfs_btree_get_leaf_keys(cur, left,
			xfs_btree_key_addr(cur, 1, new));
		xfs_btree_get_leaf_keys(cur, right,
			xfs_btree_key_addr(cur, 2, new));
	}
	xfs_btree_log_keys(cur, nbp, 1, 2);

	/* Fill in the pointer data in the new root. */
	xfs_btree_copy_ptrs(cur,
		xfs_btree_ptr_addr(cur, 1, new), &lptr, 1);
	xfs_btree_copy_ptrs(cur,
		xfs_btree_ptr_addr(cur, 2, new), &rptr, 1);
	xfs_btree_log_ptrs(cur, nbp, 1, 2);

	/* Fix up the cursor. */
	xfs_btree_setbuf(cur, cur->bc_nlevels, nbp);
	cur->bc_levels[cur->bc_nlevels].ptr = nptr;
	cur->bc_nlevels++;
	ASSERT(cur->bc_nlevels <= cur->bc_maxlevels);
	*stat = 1;
	return 0;
error0:
	return error;
out0:
	*stat = 0;
	return 0;
}

STATIC int
xfs_btree_make_block_unfull(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			level,	/* btree level */
	int			numrecs,/* # of recs in block */
	int			*oindex,/* old tree index */
	int			*index,	/* new tree index */
	union xfs_btree_ptr	*nptr,	/* new btree ptr */
	struct xfs_btree_cur	**ncur,	/* new btree cursor */
	union xfs_btree_key	*key,	/* key of new block */
	int			*stat)
{
	int			error = 0;

	if (xfs_btree_at_iroot(cur, level)) {
		struct xfs_inode *ip = cur->bc_ino.ip;

		if (numrecs < cur->bc_ops->get_dmaxrecs(cur, level)) {
			/* A root block that can be made bigger. */
			cur->bc_ops->broot_realloc(cur, numrecs + 1);
			*stat = 1;
		} else {
			/* A root block that needs replacing */
			int	logflags = 0;

			error = xfs_btree_new_iroot(cur, &logflags, stat);
			if (error || *stat == 0)
				return error;

			xfs_trans_log_inode(cur->bc_tp, ip, logflags);
		}

		return 0;
	}

	/* First, try shifting an entry to the right neighbor. */
	error = xfs_btree_rshift(cur, level, stat);
	if (error || *stat)
		return error;

	/* Next, try shifting an entry to the left neighbor. */
	error = xfs_btree_lshift(cur, level, stat);
	if (error)
		return error;

	if (*stat) {
		*oindex = *index = cur->bc_levels[level].ptr;
		return 0;
	}

	/*
	 * Next, try splitting the current block in half.
	 *
	 * If this works we have to re-set our variables because we
	 * could be in a different block now.
	 */
	error = xfs_btree_split(cur, level, nptr, key, ncur, stat);
	if (error || *stat == 0)
		return error;


	*index = cur->bc_levels[level].ptr;
	return 0;
}

/*
 * Insert one record/level.  Return information to the caller
 * allowing the next level up to proceed if necessary.
 */
STATIC int
xfs_btree_insrec(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			level,	/* level to insert record at */
	union xfs_btree_ptr	*ptrp,	/* i/o: block number inserted */
	union xfs_btree_rec	*rec,	/* record to insert */
	union xfs_btree_key	*key,	/* i/o: block key for ptrp */
	struct xfs_btree_cur	**curp,	/* output: new cursor replacing cur */
	int			*stat)	/* success/failure */
{
	struct xfs_btree_block	*block;	/* btree block */
	struct xfs_buf		*bp;	/* buffer for block */
	union xfs_btree_ptr	nptr;	/* new block ptr */
	struct xfs_btree_cur	*ncur = NULL;	/* new btree cursor */
	union xfs_btree_key	nkey;	/* new block key */
	union xfs_btree_key	*lkey;
	int			optr;	/* old key/record index */
	int			ptr;	/* key/record index */
	int			numrecs;/* number of records */
	int			error;	/* error return value */
	int			i;
	xfs_daddr_t		old_bn;

	ncur = NULL;
	lkey = &nkey;

	/*
	 * If we have an external root pointer, and we've made it to the
	 * root level, allocate a new root block and we're done.
	 */
	if (cur->bc_ops->type != XFS_BTREE_TYPE_INODE &&
	    level >= cur->bc_nlevels) {
		error = xfs_btree_new_root(cur, stat);
		xfs_btree_set_ptr_null(cur, ptrp);

		return error;
	}

	/* If we're off the left edge, return failure. */
	ptr = cur->bc_levels[level].ptr;
	if (ptr == 0) {
		*stat = 0;
		return 0;
	}

	optr = ptr;

	XFS_BTREE_STATS_INC(cur, insrec);

	/* Get pointers to the btree buffer and block. */
	block = xfs_btree_get_block(cur, level, &bp);
	old_bn = bp ? xfs_buf_daddr(bp) : XFS_BUF_DADDR_NULL;
	numrecs = xfs_btree_get_numrecs(block);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto error0;

	/* Check that the new entry is being inserted in the right place. */
	if (ptr <= numrecs) {
		if (level == 0) {
			ASSERT(cur->bc_ops->recs_inorder(cur, rec,
				xfs_btree_rec_addr(cur, ptr, block)));
		} else {
			ASSERT(cur->bc_ops->keys_inorder(cur, key,
				xfs_btree_key_addr(cur, ptr, block)));
		}
	}
#endif

	/*
	 * If the block is full, we can't insert the new entry until we
	 * make the block un-full.
	 */
	xfs_btree_set_ptr_null(cur, &nptr);
	if (numrecs == cur->bc_ops->get_maxrecs(cur, level)) {
		error = xfs_btree_make_block_unfull(cur, level, numrecs,
					&optr, &ptr, &nptr, &ncur, lkey, stat);
		if (error || *stat == 0)
			goto error0;
	}

	/*
	 * The current block may have changed if the block was
	 * previously full and we have just made space in it.
	 */
	block = xfs_btree_get_block(cur, level, &bp);
	numrecs = xfs_btree_get_numrecs(block);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto error0;
#endif

	/*
	 * At this point we know there's room for our new entry in the block
	 * we're pointing at.
	 */
	XFS_BTREE_STATS_ADD(cur, moves, numrecs - ptr + 1);

	if (level > 0) {
		/* It's a nonleaf. make a hole in the keys and ptrs */
		union xfs_btree_key	*kp;
		union xfs_btree_ptr	*pp;

		kp = xfs_btree_key_addr(cur, ptr, block);
		pp = xfs_btree_ptr_addr(cur, ptr, block);

		for (i = numrecs - ptr; i >= 0; i--) {
			error = xfs_btree_debug_check_ptr(cur, pp, i, level);
			if (error)
				goto error0;
		}

		xfs_btree_shift_keys(cur, kp, 1, numrecs - ptr + 1);
		xfs_btree_shift_ptrs(cur, pp, 1, numrecs - ptr + 1);

		error = xfs_btree_debug_check_ptr(cur, ptrp, 0, level);
		if (error)
			goto error0;

		/* Now put the new data in, bump numrecs and log it. */
		xfs_btree_copy_keys(cur, kp, key, 1);
		xfs_btree_copy_ptrs(cur, pp, ptrp, 1);
		numrecs++;
		xfs_btree_set_numrecs(block, numrecs);
		xfs_btree_log_ptrs(cur, bp, ptr, numrecs);
		xfs_btree_log_keys(cur, bp, ptr, numrecs);
#ifdef DEBUG
		if (ptr < numrecs) {
			ASSERT(cur->bc_ops->keys_inorder(cur, kp,
				xfs_btree_key_addr(cur, ptr + 1, block)));
		}
#endif
	} else {
		/* It's a leaf. make a hole in the records */
		union xfs_btree_rec             *rp;

		rp = xfs_btree_rec_addr(cur, ptr, block);

		xfs_btree_shift_recs(cur, rp, 1, numrecs - ptr + 1);

		/* Now put the new data in, bump numrecs and log it. */
		xfs_btree_copy_recs(cur, rp, rec, 1);
		xfs_btree_set_numrecs(block, ++numrecs);
		xfs_btree_log_recs(cur, bp, ptr, numrecs);
#ifdef DEBUG
		if (ptr < numrecs) {
			ASSERT(cur->bc_ops->recs_inorder(cur, rp,
				xfs_btree_rec_addr(cur, ptr + 1, block)));
		}
#endif
	}

	/* Log the new number of records in the btree header. */
	xfs_btree_log_block(cur, bp, XFS_BB_NUMRECS);

	/*
	 * Update btree keys to reflect the newly added record or keyptr.
	 * There are three cases here to be aware of.  Normally, all we have to
	 * do is walk towards the root, updating keys as necessary.
	 *
	 * If the caller had us target a full block for the insertion, we dealt
	 * with that by calling the _make_block_unfull function.  If the
	 * "make unfull" function splits the block, it'll hand us back the key
	 * and pointer of the new block.  We haven't yet added the new block to
	 * the next level up, so if we decide to add the new record to the new
	 * block (bp->b_bn != old_bn), we have to update the caller's pointer
	 * so that the caller adds the new block with the correct key.
	 *
	 * However, there is a third possibility-- if the selected block is the
	 * root block of an inode-rooted btree and cannot be expanded further,
	 * the "make unfull" function moves the root block contents to a new
	 * block and updates the root block to point to the new block.  In this
	 * case, no block pointer is passed back because the block has already
	 * been added to the btree.  In this case, we need to use the regular
	 * key update function, just like the first case.  This is critical for
	 * overlapping btrees, because the high key must be updated to reflect
	 * the entire tree, not just the subtree accessible through the first
	 * child of the root (which is now two levels down from the root).
	 */
	if (!xfs_btree_ptr_is_null(cur, &nptr) &&
	    bp && xfs_buf_daddr(bp) != old_bn) {
		xfs_btree_get_keys(cur, block, lkey);
	} else if (xfs_btree_needs_key_update(cur, optr)) {
		error = xfs_btree_update_keys(cur, level);
		if (error)
			goto error0;
	}

	/*
	 * Return the new block number, if any.
	 * If there is one, give back a record value and a cursor too.
	 */
	*ptrp = nptr;
	if (!xfs_btree_ptr_is_null(cur, &nptr)) {
		xfs_btree_copy_keys(cur, key, lkey, 1);
		*curp = ncur;
	}

	*stat = 1;
	return 0;

error0:
	if (ncur)
		xfs_btree_del_cursor(ncur, error);
	return error;
}

/*
 * Insert the record at the point referenced by cur.
 *
 * A multi-level split of the tree on insert will invalidate the original
 * cursor.  All callers of this function should assume that the cursor is
 * no longer valid and revalidate it.
 */
int
xfs_btree_insert(
	struct xfs_btree_cur	*cur,
	int			*stat)
{
	int			error;	/* error return value */
	int			i;	/* result value, 0 for failure */
	int			level;	/* current level number in btree */
	union xfs_btree_ptr	nptr;	/* new block number (split result) */
	struct xfs_btree_cur	*ncur;	/* new cursor (split result) */
	struct xfs_btree_cur	*pcur;	/* previous level's cursor */
	union xfs_btree_key	bkey;	/* key of block to insert */
	union xfs_btree_key	*key;
	union xfs_btree_rec	rec;	/* record to insert */

	level = 0;
	ncur = NULL;
	pcur = cur;
	key = &bkey;

	xfs_btree_set_ptr_null(cur, &nptr);

	/* Make a key out of the record data to be inserted, and save it. */
	cur->bc_ops->init_rec_from_cur(cur, &rec);
	cur->bc_ops->init_key_from_rec(key, &rec);

	/*
	 * Loop going up the tree, starting at the leaf level.
	 * Stop when we don't get a split block, that must mean that
	 * the insert is finished with this level.
	 */
	do {
		/*
		 * Insert nrec/nptr into this level of the tree.
		 * Note if we fail, nptr will be null.
		 */
		error = xfs_btree_insrec(pcur, level, &nptr, &rec, key,
				&ncur, &i);
		if (error) {
			if (pcur != cur)
				xfs_btree_del_cursor(pcur, XFS_BTREE_ERROR);
			goto error0;
		}

		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}
		level++;

		/*
		 * See if the cursor we just used is trash.
		 * Can't trash the caller's cursor, but otherwise we should
		 * if ncur is a new cursor or we're about to be done.
		 */
		if (pcur != cur &&
		    (ncur || xfs_btree_ptr_is_null(cur, &nptr))) {
			/* Save the state from the cursor before we trash it */
			if (cur->bc_ops->update_cursor &&
			    !(cur->bc_flags & XFS_BTREE_STAGING))
				cur->bc_ops->update_cursor(pcur, cur);
			cur->bc_nlevels = pcur->bc_nlevels;
			xfs_btree_del_cursor(pcur, XFS_BTREE_NOERROR);
		}
		/* If we got a new cursor, switch to it. */
		if (ncur) {
			pcur = ncur;
			ncur = NULL;
		}
	} while (!xfs_btree_ptr_is_null(cur, &nptr));

	*stat = i;
	return 0;
error0:
	return error;
}

/* Move the records from a child leaf block to the root block. */
STATIC void
xfs_btree_demote_leaf_child(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*cblock,
	int			numrecs)
{
	union xfs_btree_rec	*rp;
	union xfs_btree_rec	*crp;
	struct xfs_btree_block	*broot;

	/*
	 * Decrease the tree height.
	 *
	 * Trickery here: The amount of memory that we need per record for the
	 * ifork's btree root block may change when we convert the broot from a
	 * node to a leaf.  Free the old node broot so that we can get a fresh
	 * leaf broot.
	 */
	cur->bc_ops->broot_realloc(cur, 0);
	cur->bc_nlevels--;

	/*
	 * Allocate a new leaf broot and copy the records from the old child.
	 * Detach the old child from the cursor.
	 */
	broot = cur->bc_ops->broot_realloc(cur, numrecs);
	xfs_btree_init_block(cur->bc_mp, broot, cur->bc_ops, 0, numrecs,
			cur->bc_ino.ip->i_ino);

	rp = xfs_btree_rec_addr(cur, 1, broot);
	crp = xfs_btree_rec_addr(cur, 1, cblock);
	xfs_btree_copy_recs(cur, rp, crp, numrecs);

	cur->bc_levels[0].bp = NULL;
}

/*
 * Move the keyptrs from a child node block to the root block.
 *
 * Since the keyptr size does not change, all we have to do is increase the
 * tree height, copy the keyptrs to the new internal node (cblock), shrink
 * the root, and copy the pointers there.
 */
STATIC int
xfs_btree_demote_node_child(
	struct xfs_btree_cur	*cur,
	struct xfs_btree_block	*cblock,
	int			level,
	int			numrecs)
{
	struct xfs_btree_block	*block;
	union xfs_btree_key	*ckp;
	union xfs_btree_key	*kp;
	union xfs_btree_ptr	*cpp;
	union xfs_btree_ptr	*pp;
	int			i;
	int			error;

	/*
	 * Adjust the root btree node size and the record count to match the
	 * doomed child so that we can copy the keyptrs ahead of changing the
	 * tree shape.
	 */
	block = cur->bc_ops->broot_realloc(cur, numrecs);

	xfs_btree_set_numrecs(block, numrecs);
	ASSERT(block->bb_numrecs == cblock->bb_numrecs);

	/* Copy keys from the doomed block. */
	kp = xfs_btree_key_addr(cur, 1, block);
	ckp = xfs_btree_key_addr(cur, 1, cblock);
	xfs_btree_copy_keys(cur, kp, ckp, numrecs);

	/* Copy pointers from the doomed block. */
	pp = xfs_btree_ptr_addr(cur, 1, block);
	cpp = xfs_btree_ptr_addr(cur, 1, cblock);
	for (i = 0; i < numrecs; i++) {
		error = xfs_btree_debug_check_ptr(cur, cpp, i, level - 1);
		if (error)
			return error;
	}
	xfs_btree_copy_ptrs(cur, pp, cpp, numrecs);

	/* Decrease tree height, adjusting the root block level to match. */
	cur->bc_levels[level - 1].bp = NULL;
	be16_add_cpu(&block->bb_level, -1);
	cur->bc_nlevels--;
	return 0;
}

/*
 * Try to merge a non-leaf block back into the inode root.
 *
 * Note: the killroot names comes from the fact that we're effectively
 * killing the old root block.  But because we can't just delete the
 * inode we have to copy the single block it was pointing to into the
 * inode.
 */
STATIC int
xfs_btree_kill_iroot(
	struct xfs_btree_cur	*cur)
{
	struct xfs_inode	*ip = cur->bc_ino.ip;
	struct xfs_btree_block	*block;
	struct xfs_btree_block	*cblock;
	struct xfs_buf		*cbp;
	int			level;
	int			numrecs;
	int			error;
#ifdef DEBUG
	union xfs_btree_ptr	ptr;
#endif

	ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_INODE);
	ASSERT((cur->bc_ops->geom_flags & XFS_BTGEO_IROOT_RECORDS) ||
	       cur->bc_nlevels > 1);

	/*
	 * Don't deal with the root block needs to be a leaf case.
	 * We're just going to turn the thing back into extents anyway.
	 */
	level = cur->bc_nlevels - 1;
	if (level == 1 && !(cur->bc_ops->geom_flags & XFS_BTGEO_IROOT_RECORDS))
		goto out0;

	/* If we're already a leaf, jump out. */
	if (level == 0)
		goto out0;

	/*
	 * Give up if the root has multiple children.
	 */
	block = xfs_btree_get_iroot(cur);
	if (xfs_btree_get_numrecs(block) != 1)
		goto out0;

	cblock = xfs_btree_get_block(cur, level - 1, &cbp);
	numrecs = xfs_btree_get_numrecs(cblock);

	/*
	 * Only do this if the next level will fit.
	 * Then the data must be copied up to the inode,
	 * instead of freeing the root you free the next level.
	 */
	if (numrecs > cur->bc_ops->get_dmaxrecs(cur, level))
		goto out0;

	XFS_BTREE_STATS_INC(cur, killroot);

#ifdef DEBUG
	xfs_btree_get_sibling(cur, block, &ptr, XFS_BB_LEFTSIB);
	ASSERT(xfs_btree_ptr_is_null(cur, &ptr));
	xfs_btree_get_sibling(cur, block, &ptr, XFS_BB_RIGHTSIB);
	ASSERT(xfs_btree_ptr_is_null(cur, &ptr));
#endif

	if (level > 1) {
		error = xfs_btree_demote_node_child(cur, cblock, level,
				numrecs);
		if (error)
			return error;
	} else
		xfs_btree_demote_leaf_child(cur, cblock, numrecs);

	error = xfs_btree_free_block(cur, cbp);
	if (error)
		return error;

	xfs_trans_log_inode(cur->bc_tp, ip,
		XFS_ILOG_CORE | xfs_ilog_fbroot(cur->bc_ino.whichfork));
out0:
	return 0;
}

/*
 * Kill the current root node, and replace it with it's only child node.
 */
STATIC int
xfs_btree_kill_root(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp,
	int			level,
	union xfs_btree_ptr	*newroot)
{
	int			error;

	XFS_BTREE_STATS_INC(cur, killroot);

	/*
	 * Update the root pointer, decreasing the level by 1 and then
	 * free the old root.
	 */
	xfs_btree_set_root(cur, newroot, -1);

	error = xfs_btree_free_block(cur, bp);
	if (error)
		return error;

	cur->bc_levels[level].bp = NULL;
	cur->bc_levels[level].ra = 0;
	cur->bc_nlevels--;

	return 0;
}

STATIC int
xfs_btree_dec_cursor(
	struct xfs_btree_cur	*cur,
	int			level,
	int			*stat)
{
	int			error;
	int			i;

	if (level > 0) {
		error = xfs_btree_decrement(cur, level, &i);
		if (error)
			return error;
	}

	*stat = 1;
	return 0;
}

/*
 * Single level of the btree record deletion routine.
 * Delete record pointed to by cur/level.
 * Remove the record from its block then rebalance the tree.
 * Return 0 for error, 1 for done, 2 to go on to the next level.
 */
STATIC int					/* error */
xfs_btree_delrec(
	struct xfs_btree_cur	*cur,		/* btree cursor */
	int			level,		/* level removing record from */
	int			*stat)		/* fail/done/go-on */
{
	struct xfs_btree_block	*block;		/* btree block */
	union xfs_btree_ptr	cptr;		/* current block ptr */
	struct xfs_buf		*bp;		/* buffer for block */
	int			error;		/* error return value */
	int			i;		/* loop counter */
	union xfs_btree_ptr	lptr;		/* left sibling block ptr */
	struct xfs_buf		*lbp;		/* left buffer pointer */
	struct xfs_btree_block	*left;		/* left btree block */
	int			lrecs = 0;	/* left record count */
	int			ptr;		/* key/record index */
	union xfs_btree_ptr	rptr;		/* right sibling block ptr */
	struct xfs_buf		*rbp;		/* right buffer pointer */
	struct xfs_btree_block	*right;		/* right btree block */
	struct xfs_btree_block	*rrblock;	/* right-right btree block */
	struct xfs_buf		*rrbp;		/* right-right buffer pointer */
	int			rrecs = 0;	/* right record count */
	struct xfs_btree_cur	*tcur;		/* temporary btree cursor */
	int			numrecs;	/* temporary numrec count */

	tcur = NULL;

	/* Get the index of the entry being deleted, check for nothing there. */
	ptr = cur->bc_levels[level].ptr;
	if (ptr == 0) {
		*stat = 0;
		return 0;
	}

	/* Get the buffer & block containing the record or key/ptr. */
	block = xfs_btree_get_block(cur, level, &bp);
	numrecs = xfs_btree_get_numrecs(block);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto error0;
#endif

	/* Fail if we're off the end of the block. */
	if (ptr > numrecs) {
		*stat = 0;
		return 0;
	}

	XFS_BTREE_STATS_INC(cur, delrec);
	XFS_BTREE_STATS_ADD(cur, moves, numrecs - ptr);

	/* Excise the entries being deleted. */
	if (level > 0) {
		/* It's a nonleaf. operate on keys and ptrs */
		union xfs_btree_key	*lkp;
		union xfs_btree_ptr	*lpp;

		lkp = xfs_btree_key_addr(cur, ptr + 1, block);
		lpp = xfs_btree_ptr_addr(cur, ptr + 1, block);

		for (i = 0; i < numrecs - ptr; i++) {
			error = xfs_btree_debug_check_ptr(cur, lpp, i, level);
			if (error)
				goto error0;
		}

		if (ptr < numrecs) {
			xfs_btree_shift_keys(cur, lkp, -1, numrecs - ptr);
			xfs_btree_shift_ptrs(cur, lpp, -1, numrecs - ptr);
			xfs_btree_log_keys(cur, bp, ptr, numrecs - 1);
			xfs_btree_log_ptrs(cur, bp, ptr, numrecs - 1);
		}
	} else {
		/* It's a leaf. operate on records */
		if (ptr < numrecs) {
			xfs_btree_shift_recs(cur,
				xfs_btree_rec_addr(cur, ptr + 1, block),
				-1, numrecs - ptr);
			xfs_btree_log_recs(cur, bp, ptr, numrecs - 1);
		}
	}

	/*
	 * Decrement and log the number of entries in the block.
	 */
	xfs_btree_set_numrecs(block, --numrecs);
	xfs_btree_log_block(cur, bp, XFS_BB_NUMRECS);

	/*
	 * We're at the root level.  First, shrink the root block in-memory.
	 * Try to get rid of the next level down.  If we can't then there's
	 * nothing left to do.  numrecs was decremented above.
	 */
	if (xfs_btree_at_iroot(cur, level)) {
		cur->bc_ops->broot_realloc(cur, numrecs);

		error = xfs_btree_kill_iroot(cur);
		if (error)
			goto error0;

		error = xfs_btree_dec_cursor(cur, level, stat);
		if (error)
			goto error0;
		*stat = 1;
		return 0;
	}

	/*
	 * If this is the root level, and there's only one entry left, and it's
	 * NOT the leaf level, then we can get rid of this level.
	 */
	if (level == cur->bc_nlevels - 1) {
		if (numrecs == 1 && level > 0) {
			union xfs_btree_ptr	*pp;
			/*
			 * pp is still set to the first pointer in the block.
			 * Make it the new root of the btree.
			 */
			pp = xfs_btree_ptr_addr(cur, 1, block);
			error = xfs_btree_kill_root(cur, bp, level, pp);
			if (error)
				goto error0;
		} else if (level > 0) {
			error = xfs_btree_dec_cursor(cur, level, stat);
			if (error)
				goto error0;
		}
		*stat = 1;
		return 0;
	}

	/*
	 * If we deleted the leftmost entry in the block, update the
	 * key values above us in the tree.
	 */
	if (xfs_btree_needs_key_update(cur, ptr)) {
		error = xfs_btree_update_keys(cur, level);
		if (error)
			goto error0;
	}

	/*
	 * If the number of records remaining in the block is at least
	 * the minimum, we're done.
	 */
	if (numrecs >= cur->bc_ops->get_minrecs(cur, level)) {
		error = xfs_btree_dec_cursor(cur, level, stat);
		if (error)
			goto error0;
		return 0;
	}

	/*
	 * Otherwise, we have to move some records around to keep the
	 * tree balanced.  Look at the left and right sibling blocks to
	 * see if we can re-balance by moving only one record.
	 */
	xfs_btree_get_sibling(cur, block, &rptr, XFS_BB_RIGHTSIB);
	xfs_btree_get_sibling(cur, block, &lptr, XFS_BB_LEFTSIB);

	if (cur->bc_ops->type == XFS_BTREE_TYPE_INODE) {
		/*
		 * One child of root, need to get a chance to copy its contents
		 * into the root and delete it. Can't go up to next level,
		 * there's nothing to delete there.
		 */
		if (xfs_btree_ptr_is_null(cur, &rptr) &&
		    xfs_btree_ptr_is_null(cur, &lptr) &&
		    level == cur->bc_nlevels - 2) {
			error = xfs_btree_kill_iroot(cur);
			if (!error)
				error = xfs_btree_dec_cursor(cur, level, stat);
			if (error)
				goto error0;
			return 0;
		}
	}

	ASSERT(!xfs_btree_ptr_is_null(cur, &rptr) ||
	       !xfs_btree_ptr_is_null(cur, &lptr));

	/*
	 * Duplicate the cursor so our btree manipulations here won't
	 * disrupt the next level up.
	 */
	error = xfs_btree_dup_cursor(cur, &tcur);
	if (error)
		goto error0;

	/*
	 * If there's a right sibling, see if it's ok to shift an entry
	 * out of it.
	 */
	if (!xfs_btree_ptr_is_null(cur, &rptr)) {
		/*
		 * Move the temp cursor to the last entry in the next block.
		 * Actually any entry but the first would suffice.
		 */
		i = xfs_btree_lastrec(tcur, level);
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		error = xfs_btree_increment(tcur, level, &i);
		if (error)
			goto error0;
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		i = xfs_btree_lastrec(tcur, level);
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		/* Grab a pointer to the block. */
		right = xfs_btree_get_block(tcur, level, &rbp);
#ifdef DEBUG
		error = xfs_btree_check_block(tcur, right, level, rbp);
		if (error)
			goto error0;
#endif
		/* Grab the current block number, for future use. */
		xfs_btree_get_sibling(tcur, right, &cptr, XFS_BB_LEFTSIB);

		/*
		 * If right block is full enough so that removing one entry
		 * won't make it too empty, and left-shifting an entry out
		 * of right to us works, we're done.
		 */
		if (xfs_btree_get_numrecs(right) - 1 >=
		    cur->bc_ops->get_minrecs(tcur, level)) {
			error = xfs_btree_lshift(tcur, level, &i);
			if (error)
				goto error0;
			if (i) {
				ASSERT(xfs_btree_get_numrecs(block) >=
				       cur->bc_ops->get_minrecs(tcur, level));

				xfs_btree_del_cursor(tcur, XFS_BTREE_NOERROR);
				tcur = NULL;

				error = xfs_btree_dec_cursor(cur, level, stat);
				if (error)
					goto error0;
				return 0;
			}
		}

		/*
		 * Otherwise, grab the number of records in right for
		 * future reference, and fix up the temp cursor to point
		 * to our block again (last record).
		 */
		rrecs = xfs_btree_get_numrecs(right);
		if (!xfs_btree_ptr_is_null(cur, &lptr)) {
			i = xfs_btree_firstrec(tcur, level);
			if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto error0;
			}

			error = xfs_btree_decrement(tcur, level, &i);
			if (error)
				goto error0;
			if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto error0;
			}
		}
	}

	/*
	 * If there's a left sibling, see if it's ok to shift an entry
	 * out of it.
	 */
	if (!xfs_btree_ptr_is_null(cur, &lptr)) {
		/*
		 * Move the temp cursor to the first entry in the
		 * previous block.
		 */
		i = xfs_btree_firstrec(tcur, level);
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		error = xfs_btree_decrement(tcur, level, &i);
		if (error)
			goto error0;
		i = xfs_btree_firstrec(tcur, level);
		if (XFS_IS_CORRUPT(cur->bc_mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}

		/* Grab a pointer to the block. */
		left = xfs_btree_get_block(tcur, level, &lbp);
#ifdef DEBUG
		error = xfs_btree_check_block(cur, left, level, lbp);
		if (error)
			goto error0;
#endif
		/* Grab the current block number, for future use. */
		xfs_btree_get_sibling(tcur, left, &cptr, XFS_BB_RIGHTSIB);

		/*
		 * If left block is full enough so that removing one entry
		 * won't make it too empty, and right-shifting an entry out
		 * of left to us works, we're done.
		 */
		if (xfs_btree_get_numrecs(left) - 1 >=
		    cur->bc_ops->get_minrecs(tcur, level)) {
			error = xfs_btree_rshift(tcur, level, &i);
			if (error)
				goto error0;
			if (i) {
				ASSERT(xfs_btree_get_numrecs(block) >=
				       cur->bc_ops->get_minrecs(tcur, level));
				xfs_btree_del_cursor(tcur, XFS_BTREE_NOERROR);
				tcur = NULL;
				if (level == 0)
					cur->bc_levels[0].ptr++;

				*stat = 1;
				return 0;
			}
		}

		/*
		 * Otherwise, grab the number of records in right for
		 * future reference.
		 */
		lrecs = xfs_btree_get_numrecs(left);
	}

	/* Delete the temp cursor, we're done with it. */
	xfs_btree_del_cursor(tcur, XFS_BTREE_NOERROR);
	tcur = NULL;

	/* If here, we need to do a join to keep the tree balanced. */
	ASSERT(!xfs_btree_ptr_is_null(cur, &cptr));

	if (!xfs_btree_ptr_is_null(cur, &lptr) &&
	    lrecs + xfs_btree_get_numrecs(block) <=
			cur->bc_ops->get_maxrecs(cur, level)) {
		/*
		 * Set "right" to be the starting block,
		 * "left" to be the left neighbor.
		 */
		rptr = cptr;
		right = block;
		rbp = bp;
		error = xfs_btree_read_buf_block(cur, &lptr, 0, &left, &lbp);
		if (error)
			goto error0;

	/*
	 * If that won't work, see if we can join with the right neighbor block.
	 */
	} else if (!xfs_btree_ptr_is_null(cur, &rptr) &&
		   rrecs + xfs_btree_get_numrecs(block) <=
			cur->bc_ops->get_maxrecs(cur, level)) {
		/*
		 * Set "left" to be the starting block,
		 * "right" to be the right neighbor.
		 */
		lptr = cptr;
		left = block;
		lbp = bp;
		error = xfs_btree_read_buf_block(cur, &rptr, 0, &right, &rbp);
		if (error)
			goto error0;

	/*
	 * Otherwise, we can't fix the imbalance.
	 * Just return.  This is probably a logic error, but it's not fatal.
	 */
	} else {
		error = xfs_btree_dec_cursor(cur, level, stat);
		if (error)
			goto error0;
		return 0;
	}

	rrecs = xfs_btree_get_numrecs(right);
	lrecs = xfs_btree_get_numrecs(left);

	/*
	 * We're now going to join "left" and "right" by moving all the stuff
	 * in "right" to "left" and deleting "right".
	 */
	XFS_BTREE_STATS_ADD(cur, moves, rrecs);
	if (level > 0) {
		/* It's a non-leaf.  Move keys and pointers. */
		union xfs_btree_key	*lkp;	/* left btree key */
		union xfs_btree_ptr	*lpp;	/* left address pointer */
		union xfs_btree_key	*rkp;	/* right btree key */
		union xfs_btree_ptr	*rpp;	/* right address pointer */

		lkp = xfs_btree_key_addr(cur, lrecs + 1, left);
		lpp = xfs_btree_ptr_addr(cur, lrecs + 1, left);
		rkp = xfs_btree_key_addr(cur, 1, right);
		rpp = xfs_btree_ptr_addr(cur, 1, right);

		for (i = 1; i < rrecs; i++) {
			error = xfs_btree_debug_check_ptr(cur, rpp, i, level);
			if (error)
				goto error0;
		}

		xfs_btree_copy_keys(cur, lkp, rkp, rrecs);
		xfs_btree_copy_ptrs(cur, lpp, rpp, rrecs);

		xfs_btree_log_keys(cur, lbp, lrecs + 1, lrecs + rrecs);
		xfs_btree_log_ptrs(cur, lbp, lrecs + 1, lrecs + rrecs);
	} else {
		/* It's a leaf.  Move records.  */
		union xfs_btree_rec	*lrp;	/* left record pointer */
		union xfs_btree_rec	*rrp;	/* right record pointer */

		lrp = xfs_btree_rec_addr(cur, lrecs + 1, left);
		rrp = xfs_btree_rec_addr(cur, 1, right);

		xfs_btree_copy_recs(cur, lrp, rrp, rrecs);
		xfs_btree_log_recs(cur, lbp, lrecs + 1, lrecs + rrecs);
	}

	XFS_BTREE_STATS_INC(cur, join);

	/*
	 * Fix up the number of records and right block pointer in the
	 * surviving block, and log it.
	 */
	xfs_btree_set_numrecs(left, lrecs + rrecs);
	xfs_btree_get_sibling(cur, right, &cptr, XFS_BB_RIGHTSIB);
	xfs_btree_set_sibling(cur, left, &cptr, XFS_BB_RIGHTSIB);
	xfs_btree_log_block(cur, lbp, XFS_BB_NUMRECS | XFS_BB_RIGHTSIB);

	/* If there is a right sibling, point it to the remaining block. */
	xfs_btree_get_sibling(cur, left, &cptr, XFS_BB_RIGHTSIB);
	if (!xfs_btree_ptr_is_null(cur, &cptr)) {
		error = xfs_btree_read_buf_block(cur, &cptr, 0, &rrblock, &rrbp);
		if (error)
			goto error0;
		xfs_btree_set_sibling(cur, rrblock, &lptr, XFS_BB_LEFTSIB);
		xfs_btree_log_block(cur, rrbp, XFS_BB_LEFTSIB);
	}

	/* Free the deleted block. */
	error = xfs_btree_free_block(cur, rbp);
	if (error)
		goto error0;

	/*
	 * If we joined with the left neighbor, set the buffer in the
	 * cursor to the left block, and fix up the index.
	 */
	if (bp != lbp) {
		cur->bc_levels[level].bp = lbp;
		cur->bc_levels[level].ptr += lrecs;
		cur->bc_levels[level].ra = 0;
	}
	/*
	 * If we joined with the right neighbor and there's a level above
	 * us, increment the cursor at that level.
	 */
	else if (cur->bc_ops->type == XFS_BTREE_TYPE_INODE ||
		 level + 1 < cur->bc_nlevels) {
		error = xfs_btree_increment(cur, level + 1, &i);
		if (error)
			goto error0;
	}

	/*
	 * Readjust the ptr at this level if it's not a leaf, since it's
	 * still pointing at the deletion point, which makes the cursor
	 * inconsistent.  If this makes the ptr 0, the caller fixes it up.
	 * We can't use decrement because it would change the next level up.
	 */
	if (level > 0)
		cur->bc_levels[level].ptr--;

	/*
	 * We combined blocks, so we have to update the parent keys if the
	 * btree supports overlapped intervals.  However,
	 * bc_levels[level + 1].ptr points to the old block so that the caller
	 * knows which record to delete.  Therefore, the caller must be savvy
	 * enough to call updkeys for us if we return stat == 2.  The other
	 * exit points from this function don't require deletions further up
	 * the tree, so they can call updkeys directly.
	 */

	/* Return value means the next level up has something to do. */
	*stat = 2;
	return 0;

error0:
	if (tcur)
		xfs_btree_del_cursor(tcur, XFS_BTREE_ERROR);
	return error;
}

/*
 * Delete the record pointed to by cur.
 * The cursor refers to the place where the record was (could be inserted)
 * when the operation returns.
 */
int					/* error */
xfs_btree_delete(
	struct xfs_btree_cur	*cur,
	int			*stat)	/* success/failure */
{
	int			error;	/* error return value */
	int			level;
	int			i;
	bool			joined = false;

	/*
	 * Go up the tree, starting at leaf level.
	 *
	 * If 2 is returned then a join was done; go to the next level.
	 * Otherwise we are done.
	 */
	for (level = 0, i = 2; i == 2; level++) {
		error = xfs_btree_delrec(cur, level, &i);
		if (error)
			goto error0;
		if (i == 2)
			joined = true;
	}

	/*
	 * If we combined blocks as part of deleting the record, delrec won't
	 * have updated the parent high keys so we have to do that here.
	 */
	if (joined && (cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING)) {
		error = xfs_btree_updkeys_force(cur, 0);
		if (error)
			goto error0;
	}

	if (i == 0) {
		for (level = 1; level < cur->bc_nlevels; level++) {
			if (cur->bc_levels[level].ptr == 0) {
				error = xfs_btree_decrement(cur, level, &i);
				if (error)
					goto error0;
				break;
			}
		}
	}

	*stat = i;
	return 0;
error0:
	return error;
}

/*
 * Get the data from the pointed-to record.
 */
int					/* error */
xfs_btree_get_rec(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	union xfs_btree_rec	**recp,	/* output: btree record */
	int			*stat)	/* output: success/failure */
{
	struct xfs_btree_block	*block;	/* btree block */
	struct xfs_buf		*bp;	/* buffer pointer */
	int			ptr;	/* record number */
#ifdef DEBUG
	int			error;	/* error return value */
#endif

	ptr = cur->bc_levels[0].ptr;
	block = xfs_btree_get_block(cur, 0, &bp);

#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, 0, bp);
	if (error)
		return error;
#endif

	/*
	 * Off the right end or left end, return failure.
	 */
	if (ptr > xfs_btree_get_numrecs(block) || ptr <= 0) {
		*stat = 0;
		return 0;
	}

	/*
	 * Point to the record and extract its data.
	 */
	*recp = xfs_btree_rec_addr(cur, ptr, block);
	*stat = 1;
	return 0;
}

/* Visit a block in a btree. */
STATIC int
xfs_btree_visit_block(
	struct xfs_btree_cur		*cur,
	int				level,
	xfs_btree_visit_blocks_fn	fn,
	void				*data)
{
	struct xfs_btree_block		*block;
	struct xfs_buf			*bp;
	union xfs_btree_ptr		rptr, bufptr;
	int				error;

	/* do right sibling readahead */
	xfs_btree_readahead(cur, level, XFS_BTCUR_RIGHTRA);
	block = xfs_btree_get_block(cur, level, &bp);

	/* process the block */
	error = fn(cur, level, data);
	if (error)
		return error;

	/* now read rh sibling block for next iteration */
	xfs_btree_get_sibling(cur, block, &rptr, XFS_BB_RIGHTSIB);
	if (xfs_btree_ptr_is_null(cur, &rptr))
		return -ENOENT;

	/*
	 * We only visit blocks once in this walk, so we have to avoid the
	 * internal xfs_btree_lookup_get_block() optimisation where it will
	 * return the same block without checking if the right sibling points
	 * back to us and creates a cyclic reference in the btree.
	 */
	xfs_btree_buf_to_ptr(cur, bp, &bufptr);
	if (xfs_btree_ptrs_equal(cur, &rptr, &bufptr)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	return xfs_btree_lookup_get_block(cur, level, &rptr, &block);
}


/* Visit every block in a btree. */
int
xfs_btree_visit_blocks(
	struct xfs_btree_cur		*cur,
	xfs_btree_visit_blocks_fn	fn,
	unsigned int			flags,
	void				*data)
{
	union xfs_btree_ptr		lptr;
	int				level;
	struct xfs_btree_block		*block = NULL;
	int				error = 0;

	xfs_btree_init_ptr_from_cur(cur, &lptr);

	/* for each level */
	for (level = cur->bc_nlevels - 1; level >= 0; level--) {
		/* grab the left hand block */
		error = xfs_btree_lookup_get_block(cur, level, &lptr, &block);
		if (error)
			return error;

		/* readahead the left most block for the next level down */
		if (level > 0) {
			union xfs_btree_ptr     *ptr;

			ptr = xfs_btree_ptr_addr(cur, 1, block);
			xfs_btree_readahead_ptr(cur, ptr, 1);

			/* save for the next iteration of the loop */
			xfs_btree_copy_ptrs(cur, &lptr, ptr, 1);

			if (!(flags & XFS_BTREE_VISIT_LEAVES))
				continue;
		} else if (!(flags & XFS_BTREE_VISIT_RECORDS)) {
			continue;
		}

		/* for each buffer in the level */
		do {
			error = xfs_btree_visit_block(cur, level, fn, data);
		} while (!error);

		if (error != -ENOENT)
			return error;
	}

	return 0;
}

/*
 * Change the owner of a btree.
 *
 * The mechanism we use here is ordered buffer logging. Because we don't know
 * how many buffers were are going to need to modify, we don't really want to
 * have to make transaction reservations for the worst case of every buffer in a
 * full size btree as that may be more space that we can fit in the log....
 *
 * We do the btree walk in the most optimal manner possible - we have sibling
 * pointers so we can just walk all the blocks on each level from left to right
 * in a single pass, and then move to the next level and do the same. We can
 * also do readahead on the sibling pointers to get IO moving more quickly,
 * though for slow disks this is unlikely to make much difference to performance
 * as the amount of CPU work we have to do before moving to the next block is
 * relatively small.
 *
 * For each btree block that we load, modify the owner appropriately, set the
 * buffer as an ordered buffer and log it appropriately. We need to ensure that
 * we mark the region we change dirty so that if the buffer is relogged in
 * a subsequent transaction the changes we make here as an ordered buffer are
 * correctly relogged in that transaction.  If we are in recovery context, then
 * just queue the modified buffer as delayed write buffer so the transaction
 * recovery completion writes the changes to disk.
 */
struct xfs_btree_block_change_owner_info {
	uint64_t		new_owner;
	struct list_head	*buffer_list;
};

static int
xfs_btree_block_change_owner(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*data)
{
	struct xfs_btree_block_change_owner_info	*bbcoi = data;
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;

	/* modify the owner */
	block = xfs_btree_get_block(cur, level, &bp);
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN) {
		if (block->bb_u.l.bb_owner == cpu_to_be64(bbcoi->new_owner))
			return 0;
		block->bb_u.l.bb_owner = cpu_to_be64(bbcoi->new_owner);
	} else {
		if (block->bb_u.s.bb_owner == cpu_to_be32(bbcoi->new_owner))
			return 0;
		block->bb_u.s.bb_owner = cpu_to_be32(bbcoi->new_owner);
	}

	/*
	 * If the block is a root block hosted in an inode, we might not have a
	 * buffer pointer here and we shouldn't attempt to log the change as the
	 * information is already held in the inode and discarded when the root
	 * block is formatted into the on-disk inode fork. We still change it,
	 * though, so everything is consistent in memory.
	 */
	if (!bp) {
		ASSERT(cur->bc_ops->type == XFS_BTREE_TYPE_INODE);
		ASSERT(level == cur->bc_nlevels - 1);
		return 0;
	}

	if (cur->bc_tp) {
		if (!xfs_trans_ordered_buf(cur->bc_tp, bp)) {
			xfs_btree_log_block(cur, bp, XFS_BB_OWNER);
			return -EAGAIN;
		}
	} else {
		xfs_buf_delwri_queue(bp, bbcoi->buffer_list);
	}

	return 0;
}

int
xfs_btree_change_owner(
	struct xfs_btree_cur	*cur,
	uint64_t		new_owner,
	struct list_head	*buffer_list)
{
	struct xfs_btree_block_change_owner_info	bbcoi;

	bbcoi.new_owner = new_owner;
	bbcoi.buffer_list = buffer_list;

	return xfs_btree_visit_blocks(cur, xfs_btree_block_change_owner,
			XFS_BTREE_VISIT_ALL, &bbcoi);
}

/* Verify the v5 fields of a long-format btree block. */
xfs_failaddr_t
xfs_btree_fsblock_v5hdr_verify(
	struct xfs_buf		*bp,
	uint64_t		owner)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);

	if (!xfs_has_crc(mp))
		return __this_address;
	if (!uuid_equal(&block->bb_u.l.bb_uuid, &mp->m_sb.sb_meta_uuid))
		return __this_address;
	if (block->bb_u.l.bb_blkno != cpu_to_be64(xfs_buf_daddr(bp)))
		return __this_address;
	if (owner != XFS_RMAP_OWN_UNKNOWN &&
	    be64_to_cpu(block->bb_u.l.bb_owner) != owner)
		return __this_address;
	return NULL;
}

/* Verify a long-format btree block. */
xfs_failaddr_t
xfs_btree_fsblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_fsblock_t		fsb;
	xfs_failaddr_t		fa;

	ASSERT(!xfs_buftarg_is_mem(bp->b_target));

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	fsb = XFS_DADDR_TO_FSB(mp, xfs_buf_daddr(bp));
	fa = xfs_btree_check_fsblock_siblings(mp, fsb,
			block->bb_u.l.bb_leftsib);
	if (!fa)
		fa = xfs_btree_check_fsblock_siblings(mp, fsb,
				block->bb_u.l.bb_rightsib);
	return fa;
}

/* Verify an in-memory btree block. */
xfs_failaddr_t
xfs_btree_memblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_buftarg	*btp = bp->b_target;
	xfs_failaddr_t		fa;
	xfbno_t			bno;

	ASSERT(xfs_buftarg_is_mem(bp->b_target));

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	bno = xfs_daddr_to_xfbno(xfs_buf_daddr(bp));
	fa = xfs_btree_check_memblock_siblings(btp, bno,
			block->bb_u.l.bb_leftsib);
	if (fa)
		return fa;
	fa = xfs_btree_check_memblock_siblings(btp, bno,
			block->bb_u.l.bb_rightsib);
	if (fa)
		return fa;

	return NULL;
}
/**
 * xfs_btree_agblock_v5hdr_verify() -- verify the v5 fields of a short-format
 *				      btree block
 *
 * @bp: buffer containing the btree block
 */
xfs_failaddr_t
xfs_btree_agblock_v5hdr_verify(
	struct xfs_buf		*bp)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	struct xfs_perag	*pag = bp->b_pag;

	if (!xfs_has_crc(mp))
		return __this_address;
	if (!uuid_equal(&block->bb_u.s.bb_uuid, &mp->m_sb.sb_meta_uuid))
		return __this_address;
	if (block->bb_u.s.bb_blkno != cpu_to_be64(xfs_buf_daddr(bp)))
		return __this_address;
	if (pag && be32_to_cpu(block->bb_u.s.bb_owner) != pag_agno(pag))
		return __this_address;
	return NULL;
}

/**
 * xfs_btree_agblock_verify() -- verify a short-format btree block
 *
 * @bp: buffer containing the btree block
 * @max_recs: maximum records allowed in this btree node
 */
xfs_failaddr_t
xfs_btree_agblock_verify(
	struct xfs_buf		*bp,
	unsigned int		max_recs)
{
	struct xfs_mount	*mp = bp->b_mount;
	struct xfs_btree_block	*block = XFS_BUF_TO_BLOCK(bp);
	xfs_agblock_t		agbno;
	xfs_failaddr_t		fa;

	ASSERT(!xfs_buftarg_is_mem(bp->b_target));

	/* numrecs verification */
	if (be16_to_cpu(block->bb_numrecs) > max_recs)
		return __this_address;

	/* sibling pointer verification */
	agbno = xfs_daddr_to_agbno(mp, xfs_buf_daddr(bp));
	fa = xfs_btree_check_agblock_siblings(bp->b_pag, agbno,
			block->bb_u.s.bb_leftsib);
	if (!fa)
		fa = xfs_btree_check_agblock_siblings(bp->b_pag, agbno,
				block->bb_u.s.bb_rightsib);
	return fa;
}

/*
 * For the given limits on leaf and keyptr records per block, calculate the
 * height of the tree needed to index the number of leaf records.
 */
unsigned int
xfs_btree_compute_maxlevels(
	const unsigned int	*limits,
	unsigned long long	records)
{
	unsigned long long	level_blocks = howmany_64(records, limits[0]);
	unsigned int		height = 1;

	while (level_blocks > 1) {
		level_blocks = howmany_64(level_blocks, limits[1]);
		height++;
	}

	return height;
}

/*
 * For the given limits on leaf and keyptr records per block, calculate the
 * number of blocks needed to index the given number of leaf records.
 */
unsigned long long
xfs_btree_calc_size(
	const unsigned int	*limits,
	unsigned long long	records)
{
	unsigned long long	level_blocks = howmany_64(records, limits[0]);
	unsigned long long	blocks = level_blocks;

	while (level_blocks > 1) {
		level_blocks = howmany_64(level_blocks, limits[1]);
		blocks += level_blocks;
	}

	return blocks;
}

/*
 * Given a number of available blocks for the btree to consume with records and
 * pointers, calculate the height of the tree needed to index all the records
 * that space can hold based on the number of pointers each interior node
 * holds.
 *
 * We start by assuming a single level tree consumes a single block, then track
 * the number of blocks each node level consumes until we no longer have space
 * to store the next node level. At this point, we are indexing all the leaf
 * blocks in the space, and there's no more free space to split the tree any
 * further. That's our maximum btree height.
 */
unsigned int
xfs_btree_space_to_height(
	const unsigned int	*limits,
	unsigned long long	leaf_blocks)
{
	/*
	 * The root btree block can have fewer than minrecs pointers in it
	 * because the tree might not be big enough to require that amount of
	 * fanout. Hence it has a minimum size of 2 pointers, not limits[1].
	 */
	unsigned long long	node_blocks = 2;
	unsigned long long	blocks_left = leaf_blocks - 1;
	unsigned int		height = 1;

	if (leaf_blocks < 1)
		return 0;

	while (node_blocks < blocks_left) {
		blocks_left -= node_blocks;
		node_blocks *= limits[1];
		height++;
	}

	return height;
}

/*
 * Query a regular btree for all records overlapping a given interval.
 * Start with a LE lookup of the key of low_rec and return all records
 * until we find a record with a key greater than the key of high_rec.
 */
STATIC int
xfs_btree_simple_query_range(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*low_key,
	const union xfs_btree_key	*high_key,
	xfs_btree_query_range_fn	fn,
	void				*priv)
{
	union xfs_btree_rec		*recp;
	union xfs_btree_key		rec_key;
	int				stat;
	bool				firstrec = true;
	int				error;

	ASSERT(cur->bc_ops->init_high_key_from_rec);
	ASSERT(cur->bc_ops->cmp_two_keys);

	/*
	 * Find the leftmost record.  The btree cursor must be set
	 * to the low record used to generate low_key.
	 */
	stat = 0;
	error = xfs_btree_lookup(cur, XFS_LOOKUP_LE, &stat);
	if (error)
		goto out;

	/* Nothing?  See if there's anything to the right. */
	if (!stat) {
		error = xfs_btree_increment(cur, 0, &stat);
		if (error)
			goto out;
	}

	while (stat) {
		/* Find the record. */
		error = xfs_btree_get_rec(cur, &recp, &stat);
		if (error || !stat)
			break;

		/* Skip if low_key > high_key(rec). */
		if (firstrec) {
			cur->bc_ops->init_high_key_from_rec(&rec_key, recp);
			firstrec = false;
			if (xfs_btree_keycmp_gt(cur, low_key, &rec_key))
				goto advloop;
		}

		/* Stop if low_key(rec) > high_key. */
		cur->bc_ops->init_key_from_rec(&rec_key, recp);
		if (xfs_btree_keycmp_gt(cur, &rec_key, high_key))
			break;

		/* Callback */
		error = fn(cur, recp, priv);
		if (error)
			break;

advloop:
		/* Move on to the next record. */
		error = xfs_btree_increment(cur, 0, &stat);
		if (error)
			break;
	}

out:
	return error;
}

/*
 * Query an overlapped interval btree for all records overlapping a given
 * interval.  This function roughly follows the algorithm given in
 * "Interval Trees" of _Introduction to Algorithms_, which is section
 * 14.3 in the 2nd and 3rd editions.
 *
 * First, generate keys for the low and high records passed in.
 *
 * For any leaf node, generate the high and low keys for the record.
 * If the record keys overlap with the query low/high keys, pass the
 * record to the function iterator.
 *
 * For any internal node, compare the low and high keys of each
 * pointer against the query low/high keys.  If there's an overlap,
 * follow the pointer.
 *
 * As an optimization, we stop scanning a block when we find a low key
 * that is greater than the query's high key.
 */
STATIC int
xfs_btree_overlapped_query_range(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*low_key,
	const union xfs_btree_key	*high_key,
	xfs_btree_query_range_fn	fn,
	void				*priv)
{
	union xfs_btree_ptr		ptr;
	union xfs_btree_ptr		*pp;
	union xfs_btree_key		rec_key;
	union xfs_btree_key		rec_hkey;
	union xfs_btree_key		*lkp;
	union xfs_btree_key		*hkp;
	union xfs_btree_rec		*recp;
	struct xfs_btree_block		*block;
	int				level;
	struct xfs_buf			*bp;
	int				i;
	int				error;

	/* Load the root of the btree. */
	level = cur->bc_nlevels - 1;
	xfs_btree_init_ptr_from_cur(cur, &ptr);
	error = xfs_btree_lookup_get_block(cur, level, &ptr, &block);
	if (error)
		return error;
	xfs_btree_get_block(cur, level, &bp);
	trace_xfs_btree_overlapped_query_range(cur, level, bp);
#ifdef DEBUG
	error = xfs_btree_check_block(cur, block, level, bp);
	if (error)
		goto out;
#endif
	cur->bc_levels[level].ptr = 1;

	while (level < cur->bc_nlevels) {
		block = xfs_btree_get_block(cur, level, &bp);

		/* End of node, pop back towards the root. */
		if (cur->bc_levels[level].ptr >
					be16_to_cpu(block->bb_numrecs)) {
pop_up:
			if (level < cur->bc_nlevels - 1)
				cur->bc_levels[level + 1].ptr++;
			level++;
			continue;
		}

		if (level == 0) {
			/* Handle a leaf node. */
			recp = xfs_btree_rec_addr(cur, cur->bc_levels[0].ptr,
					block);

			cur->bc_ops->init_high_key_from_rec(&rec_hkey, recp);
			cur->bc_ops->init_key_from_rec(&rec_key, recp);

			/*
			 * If (query's high key < record's low key), then there
			 * are no more interesting records in this block.  Pop
			 * up to the leaf level to find more record blocks.
			 *
			 * If (record's high key >= query's low key) and
			 *    (query's high key >= record's low key), then
			 * this record overlaps the query range; callback.
			 */
			if (xfs_btree_keycmp_lt(cur, high_key, &rec_key))
				goto pop_up;
			if (xfs_btree_keycmp_ge(cur, &rec_hkey, low_key)) {
				error = fn(cur, recp, priv);
				if (error)
					break;
			}
			cur->bc_levels[level].ptr++;
			continue;
		}

		/* Handle an internal node. */
		lkp = xfs_btree_key_addr(cur, cur->bc_levels[level].ptr, block);
		hkp = xfs_btree_high_key_addr(cur, cur->bc_levels[level].ptr,
				block);
		pp = xfs_btree_ptr_addr(cur, cur->bc_levels[level].ptr, block);

		/*
		 * If (query's high key < pointer's low key), then there are no
		 * more interesting keys in this block.  Pop up one leaf level
		 * to continue looking for records.
		 *
		 * If (pointer's high key >= query's low key) and
		 *    (query's high key >= pointer's low key), then
		 * this record overlaps the query range; follow pointer.
		 */
		if (xfs_btree_keycmp_lt(cur, high_key, lkp))
			goto pop_up;
		if (xfs_btree_keycmp_ge(cur, hkp, low_key)) {
			level--;
			error = xfs_btree_lookup_get_block(cur, level, pp,
					&block);
			if (error)
				goto out;
			xfs_btree_get_block(cur, level, &bp);
			trace_xfs_btree_overlapped_query_range(cur, level, bp);
#ifdef DEBUG
			error = xfs_btree_check_block(cur, block, level, bp);
			if (error)
				goto out;
#endif
			cur->bc_levels[level].ptr = 1;
			continue;
		}
		cur->bc_levels[level].ptr++;
	}

out:
	/*
	 * If we don't end this function with the cursor pointing at a record
	 * block, a subsequent non-error cursor deletion will not release
	 * node-level buffers, causing a buffer leak.  This is quite possible
	 * with a zero-results range query, so release the buffers if we
	 * failed to return any results.
	 */
	if (cur->bc_levels[0].bp == NULL) {
		for (i = 0; i < cur->bc_nlevels; i++) {
			if (cur->bc_levels[i].bp) {
				xfs_trans_brelse(cur->bc_tp,
						cur->bc_levels[i].bp);
				cur->bc_levels[i].bp = NULL;
				cur->bc_levels[i].ptr = 0;
				cur->bc_levels[i].ra = 0;
			}
		}
	}

	return error;
}

static inline void
xfs_btree_key_from_irec(
	struct xfs_btree_cur		*cur,
	union xfs_btree_key		*key,
	const union xfs_btree_irec	*irec)
{
	union xfs_btree_rec		rec;

	cur->bc_rec = *irec;
	cur->bc_ops->init_rec_from_cur(cur, &rec);
	cur->bc_ops->init_key_from_rec(key, &rec);
}

/*
 * Query a btree for all records overlapping a given interval of keys.  The
 * supplied function will be called with each record found; return one of the
 * XFS_BTREE_QUERY_RANGE_{CONTINUE,ABORT} values or the usual negative error
 * code.  This function returns -ECANCELED, zero, or a negative error code.
 */
int
xfs_btree_query_range(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_irec	*low_rec,
	const union xfs_btree_irec	*high_rec,
	xfs_btree_query_range_fn	fn,
	void				*priv)
{
	union xfs_btree_key		low_key;
	union xfs_btree_key		high_key;

	/* Find the keys of both ends of the interval. */
	xfs_btree_key_from_irec(cur, &high_key, high_rec);
	xfs_btree_key_from_irec(cur, &low_key, low_rec);

	/* Enforce low key <= high key. */
	if (!xfs_btree_keycmp_le(cur, &low_key, &high_key))
		return -EINVAL;

	if (!(cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING))
		return xfs_btree_simple_query_range(cur, &low_key,
				&high_key, fn, priv);
	return xfs_btree_overlapped_query_range(cur, &low_key, &high_key,
			fn, priv);
}

/* Query a btree for all records. */
int
xfs_btree_query_all(
	struct xfs_btree_cur		*cur,
	xfs_btree_query_range_fn	fn,
	void				*priv)
{
	union xfs_btree_key		low_key;
	union xfs_btree_key		high_key;

	memset(&cur->bc_rec, 0, sizeof(cur->bc_rec));
	memset(&low_key, 0, sizeof(low_key));
	memset(&high_key, 0xFF, sizeof(high_key));

	return xfs_btree_simple_query_range(cur, &low_key, &high_key, fn, priv);
}

static int
xfs_btree_count_blocks_helper(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*data)
{
	xfs_filblks_t		*blocks = data;
	(*blocks)++;

	return 0;
}

/* Count the blocks in a btree and return the result in *blocks. */
int
xfs_btree_count_blocks(
	struct xfs_btree_cur	*cur,
	xfs_filblks_t		*blocks)
{
	*blocks = 0;
	return xfs_btree_visit_blocks(cur, xfs_btree_count_blocks_helper,
			XFS_BTREE_VISIT_ALL, blocks);
}

/* Compare two btree pointers. */
int
xfs_btree_cmp_two_ptrs(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*a,
	const union xfs_btree_ptr	*b)
{
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		return cmp_int(be64_to_cpu(a->l), be64_to_cpu(b->l));
	return cmp_int(be32_to_cpu(a->s), be32_to_cpu(b->s));
}

struct xfs_btree_has_records {
	/* Keys for the start and end of the range we want to know about. */
	union xfs_btree_key		start_key;
	union xfs_btree_key		end_key;

	/* Mask for key comparisons, if desired. */
	const union xfs_btree_key	*key_mask;

	/* Highest record key we've seen so far. */
	union xfs_btree_key		high_key;

	enum xbtree_recpacking		outcome;
};

STATIC int
xfs_btree_has_records_helper(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*rec,
	void				*priv)
{
	union xfs_btree_key		rec_key;
	union xfs_btree_key		rec_high_key;
	struct xfs_btree_has_records	*info = priv;
	enum xbtree_key_contig		key_contig;

	cur->bc_ops->init_key_from_rec(&rec_key, rec);

	if (info->outcome == XBTREE_RECPACKING_EMPTY) {
		info->outcome = XBTREE_RECPACKING_SPARSE;

		/*
		 * If the first record we find does not overlap the start key,
		 * then there is a hole at the start of the search range.
		 * Classify this as sparse and stop immediately.
		 */
		if (xfs_btree_masked_keycmp_lt(cur, &info->start_key, &rec_key,
					info->key_mask))
			return -ECANCELED;
	} else {
		/*
		 * If a subsequent record does not overlap with the any record
		 * we've seen so far, there is a hole in the middle of the
		 * search range.  Classify this as sparse and stop.
		 * If the keys overlap and this btree does not allow overlap,
		 * signal corruption.
		 */
		key_contig = cur->bc_ops->keys_contiguous(cur, &info->high_key,
					&rec_key, info->key_mask);
		if (key_contig == XBTREE_KEY_OVERLAP &&
				!(cur->bc_ops->geom_flags & XFS_BTGEO_OVERLAPPING))
			return -EFSCORRUPTED;
		if (key_contig == XBTREE_KEY_GAP)
			return -ECANCELED;
	}

	/*
	 * If high_key(rec) is larger than any other high key we've seen,
	 * remember it for later.
	 */
	cur->bc_ops->init_high_key_from_rec(&rec_high_key, rec);
	if (xfs_btree_masked_keycmp_gt(cur, &rec_high_key, &info->high_key,
				info->key_mask))
		info->high_key = rec_high_key; /* struct copy */

	return 0;
}

/*
 * Scan part of the keyspace of a btree and tell us if that keyspace does not
 * map to any records; is fully mapped to records; or is partially mapped to
 * records.  This is the btree record equivalent to determining if a file is
 * sparse.
 *
 * For most btree types, the record scan should use all available btree key
 * fields to compare the keys encountered.  These callers should pass NULL for
 * @mask.  However, some callers (e.g.  scanning physical space in the rmapbt)
 * want to ignore some part of the btree record keyspace when performing the
 * comparison.  These callers should pass in a union xfs_btree_key object with
 * the fields that *should* be a part of the comparison set to any nonzero
 * value, and the rest zeroed.
 */
int
xfs_btree_has_records(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_irec	*low,
	const union xfs_btree_irec	*high,
	const union xfs_btree_key	*mask,
	enum xbtree_recpacking		*outcome)
{
	struct xfs_btree_has_records	info = {
		.outcome		= XBTREE_RECPACKING_EMPTY,
		.key_mask		= mask,
	};
	int				error;

	/* Not all btrees support this operation. */
	if (!cur->bc_ops->keys_contiguous) {
		ASSERT(0);
		return -EOPNOTSUPP;
	}

	xfs_btree_key_from_irec(cur, &info.start_key, low);
	xfs_btree_key_from_irec(cur, &info.end_key, high);

	error = xfs_btree_query_range(cur, low, high,
			xfs_btree_has_records_helper, &info);
	if (error == -ECANCELED)
		goto out;
	if (error)
		return error;

	if (info.outcome == XBTREE_RECPACKING_EMPTY)
		goto out;

	/*
	 * If the largest high_key(rec) we saw during the walk is greater than
	 * the end of the search range, classify this as full.  Otherwise,
	 * there is a hole at the end of the search range.
	 */
	if (xfs_btree_masked_keycmp_ge(cur, &info.high_key, &info.end_key,
				mask))
		info.outcome = XBTREE_RECPACKING_FULL;

out:
	*outcome = info.outcome;
	return 0;
}

/* Are there more records in this btree? */
bool
xfs_btree_has_more_records(
	struct xfs_btree_cur	*cur)
{
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;

	block = xfs_btree_get_block(cur, 0, &bp);

	/* There are still records in this block. */
	if (cur->bc_levels[0].ptr < xfs_btree_get_numrecs(block))
		return true;

	/* There are more record blocks. */
	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		return block->bb_u.l.bb_rightsib != cpu_to_be64(NULLFSBLOCK);
	else
		return block->bb_u.s.bb_rightsib != cpu_to_be32(NULLAGBLOCK);
}

/* Set up all the btree cursor caches. */
int __init
xfs_btree_init_cur_caches(void)
{
	int		error;

	error = xfs_allocbt_init_cur_cache();
	if (error)
		return error;
	error = xfs_inobt_init_cur_cache();
	if (error)
		goto err;
	error = xfs_bmbt_init_cur_cache();
	if (error)
		goto err;
	error = xfs_rmapbt_init_cur_cache();
	if (error)
		goto err;
	error = xfs_refcountbt_init_cur_cache();
	if (error)
		goto err;
	error = xfs_rtrmapbt_init_cur_cache();
	if (error)
		goto err;
	error = xfs_rtrefcountbt_init_cur_cache();
	if (error)
		goto err;

	return 0;
err:
	xfs_btree_destroy_cur_caches();
	return error;
}

/* Destroy all the btree cursor caches, if they've been allocated. */
void
xfs_btree_destroy_cur_caches(void)
{
	xfs_allocbt_destroy_cur_cache();
	xfs_inobt_destroy_cur_cache();
	xfs_bmbt_destroy_cur_cache();
	xfs_rmapbt_destroy_cur_cache();
	xfs_refcountbt_destroy_cur_cache();
	xfs_rtrmapbt_destroy_cur_cache();
	xfs_rtrefcountbt_destroy_cur_cache();
}

/* Move the btree cursor before the first record. */
int
xfs_btree_goto_left_edge(
	struct xfs_btree_cur	*cur)
{
	int			stat = 0;
	int			error;

	memset(&cur->bc_rec, 0, sizeof(cur->bc_rec));
	error = xfs_btree_lookup(cur, XFS_LOOKUP_LE, &stat);
	if (error)
		return error;
	if (!stat)
		return 0;

	error = xfs_btree_decrement(cur, 0, &stat);
	if (error)
		return error;
	if (stat != 0) {
		ASSERT(0);
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	return 0;
}

/* Allocate a block for an inode-rooted metadata btree. */
int
xfs_btree_alloc_metafile_block(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_ptr	*start,
	union xfs_btree_ptr		*new,
	int				*stat)
{
	struct xfs_alloc_arg		args = {
		.mp			= cur->bc_mp,
		.tp			= cur->bc_tp,
		.resv			= XFS_AG_RESV_METAFILE,
		.minlen			= 1,
		.maxlen			= 1,
		.prod			= 1,
	};
	struct xfs_inode		*ip = cur->bc_ino.ip;
	int				error;

	ASSERT(xfs_is_metadir_inode(ip));

	xfs_rmap_ino_bmbt_owner(&args.oinfo, ip->i_ino, cur->bc_ino.whichfork);
	error = xfs_alloc_vextent_start_ag(&args,
			XFS_INO_TO_FSB(cur->bc_mp, ip->i_ino));
	if (error)
		return error;
	if (args.fsbno == NULLFSBLOCK) {
		*stat = 0;
		return 0;
	}
	ASSERT(args.len == 1);

	xfs_metafile_resv_alloc_space(ip, &args);

	new->l = cpu_to_be64(args.fsbno);
	*stat = 1;
	return 0;
}

/* Free a block from an inode-rooted metadata btree. */
int
xfs_btree_free_metafile_block(
	struct xfs_btree_cur	*cur,
	struct xfs_buf		*bp)
{
	struct xfs_owner_info	oinfo;
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_inode	*ip = cur->bc_ino.ip;
	struct xfs_trans	*tp = cur->bc_tp;
	xfs_fsblock_t		fsbno = XFS_DADDR_TO_FSB(mp, xfs_buf_daddr(bp));
	int			error;

	ASSERT(xfs_is_metadir_inode(ip));

	xfs_rmap_ino_bmbt_owner(&oinfo, ip->i_ino, cur->bc_ino.whichfork);
	error = xfs_free_extent_later(tp, fsbno, 1, &oinfo, XFS_AG_RESV_METAFILE,
			0);
	if (error)
		return error;

	xfs_metafile_resv_free_space(ip, tp, 1);
	return 0;
}
