/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_da_format.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_trans.h"
#include "xfs_extfree_item.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_bmap_btree.h"
#include "xfs_rtalloc.h"
#include "xfs_error.h"
#include "xfs_quota.h"
#include "xfs_trans_space.h"
#include "xfs_trace.h"
#include "xfs_icache.h"
#include "xfs_log.h"

/* Kernel only BMAP related definitions and functions */

/*
 * Convert the given file system block to a disk block.  We have to treat it
 * differently based on whether the file is a real time file or not, because the
 * bmap code does.
 */
xfs_daddr_t
xfs_fsb_to_db(struct xfs_inode *ip, xfs_fsblock_t fsb)
{
	return (XFS_IS_REALTIME_INODE(ip) ? \
		 (xfs_daddr_t)XFS_FSB_TO_BB((ip)->i_mount, (fsb)) : \
		 XFS_FSB_TO_DADDR((ip)->i_mount, (fsb)));
}

/*
 * Routine to zero an extent on disk allocated to the specific inode.
 *
 * The VFS functions take a linearised filesystem block offset, so we have to
 * convert the sparse xfs fsb to the right format first.
 * VFS types are real funky, too.
 */
int
xfs_zero_extent(
	struct xfs_inode *ip,
	xfs_fsblock_t	start_fsb,
	xfs_off_t	count_fsb)
{
	struct xfs_mount *mp = ip->i_mount;
	xfs_daddr_t	sector = xfs_fsb_to_db(ip, start_fsb);
	sector_t	block = XFS_BB_TO_FSBT(mp, sector);

	return blkdev_issue_zeroout(xfs_find_bdev_for_inode(VFS_I(ip)),
		block << (mp->m_super->s_blocksize_bits - 9),
		count_fsb << (mp->m_super->s_blocksize_bits - 9),
		GFP_NOFS);
}

int
xfs_bmap_rtalloc(
	struct xfs_bmalloca	*ap)	/* bmap alloc argument struct */
{
	int		error;		/* error return value */
	xfs_mount_t	*mp;		/* mount point structure */
	xfs_extlen_t	prod = 0;	/* product factor for allocators */
	xfs_extlen_t	ralen = 0;	/* realtime allocation length */
	xfs_extlen_t	align;		/* minimum allocation alignment */
	xfs_rtblock_t	rtb;

	mp = ap->ip->i_mount;
	align = xfs_get_extsz_hint(ap->ip);
	prod = align / mp->m_sb.sb_rextsize;
	error = xfs_bmap_extsize_align(mp, &ap->got, &ap->prev,
					align, 1, ap->eof, 0,
					ap->conv, &ap->offset, &ap->length);
	if (error)
		return error;
	ASSERT(ap->length);
	ASSERT(ap->length % mp->m_sb.sb_rextsize == 0);

	/*
	 * If the offset & length are not perfectly aligned
	 * then kill prod, it will just get us in trouble.
	 */
	if (do_mod(ap->offset, align) || ap->length % align)
		prod = 1;
	/*
	 * Set ralen to be the actual requested length in rtextents.
	 */
	ralen = ap->length / mp->m_sb.sb_rextsize;
	/*
	 * If the old value was close enough to MAXEXTLEN that
	 * we rounded up to it, cut it back so it's valid again.
	 * Note that if it's a really large request (bigger than
	 * MAXEXTLEN), we don't hear about that number, and can't
	 * adjust the starting point to match it.
	 */
	if (ralen * mp->m_sb.sb_rextsize >= MAXEXTLEN)
		ralen = MAXEXTLEN / mp->m_sb.sb_rextsize;

	/*
	 * Lock out modifications to both the RT bitmap and summary inodes
	 */
	xfs_ilock(mp->m_rbmip, XFS_ILOCK_EXCL|XFS_ILOCK_RTBITMAP);
	xfs_trans_ijoin(ap->tp, mp->m_rbmip, XFS_ILOCK_EXCL);
	xfs_ilock(mp->m_rsumip, XFS_ILOCK_EXCL|XFS_ILOCK_RTSUM);
	xfs_trans_ijoin(ap->tp, mp->m_rsumip, XFS_ILOCK_EXCL);

	/*
	 * If it's an allocation to an empty file at offset 0,
	 * pick an extent that will space things out in the rt area.
	 */
	if (ap->eof && ap->offset == 0) {
		xfs_rtblock_t uninitialized_var(rtx); /* realtime extent no */

		error = xfs_rtpick_extent(mp, ap->tp, ralen, &rtx);
		if (error)
			return error;
		ap->blkno = rtx * mp->m_sb.sb_rextsize;
	} else {
		ap->blkno = 0;
	}

	xfs_bmap_adjacent(ap);

	/*
	 * Realtime allocation, done through xfs_rtallocate_extent.
	 */
	do_div(ap->blkno, mp->m_sb.sb_rextsize);
	rtb = ap->blkno;
	ap->length = ralen;
	error = xfs_rtallocate_extent(ap->tp, ap->blkno, 1, ap->length,
				&ralen, ap->wasdel, prod, &rtb);
	if (error)
		return error;

	ap->blkno = rtb;
	if (ap->blkno != NULLFSBLOCK) {
		ap->blkno *= mp->m_sb.sb_rextsize;
		ralen *= mp->m_sb.sb_rextsize;
		ap->length = ralen;
		ap->ip->i_d.di_nblocks += ralen;
		xfs_trans_log_inode(ap->tp, ap->ip, XFS_ILOG_CORE);
		if (ap->wasdel)
			ap->ip->i_delayed_blks -= ralen;
		/*
		 * Adjust the disk quota also. This was reserved
		 * earlier.
		 */
		xfs_trans_mod_dquot_byino(ap->tp, ap->ip,
			ap->wasdel ? XFS_TRANS_DQ_DELRTBCOUNT :
					XFS_TRANS_DQ_RTBCOUNT, (long) ralen);

		/* Zero the extent if we were asked to do so */
		if (ap->datatype & XFS_ALLOC_USERDATA_ZERO) {
			error = xfs_zero_extent(ap->ip, ap->blkno, ap->length);
			if (error)
				return error;
		}
	} else {
		ap->length = 0;
	}
	return 0;
}

/*
 * Check if the endoff is outside the last extent. If so the caller will grow
 * the allocation to a stripe unit boundary.  All offsets are considered outside
 * the end of file for an empty fork, so 1 is returned in *eof in that case.
 */
int
xfs_bmap_eof(
	struct xfs_inode	*ip,
	xfs_fileoff_t		endoff,
	int			whichfork,
	int			*eof)
{
	struct xfs_bmbt_irec	rec;
	int			error;

	error = xfs_bmap_last_extent(NULL, ip, whichfork, &rec, eof);
	if (error || *eof)
		return error;

	*eof = endoff >= rec.br_startoff + rec.br_blockcount;
	return 0;
}

/*
 * Extent tree block counting routines.
 */

/*
 * Count leaf blocks given a range of extent records.  Delayed allocation
 * extents are not counted towards the totals.
 */
STATIC void
xfs_bmap_count_leaves(
	struct xfs_ifork	*ifp,
	xfs_extnum_t		*numrecs,
	xfs_filblks_t		*count)
{
	xfs_extnum_t		i;
	xfs_extnum_t		nr_exts = xfs_iext_count(ifp);

	for (i = 0; i < nr_exts; i++) {
		xfs_bmbt_rec_host_t *frp = xfs_iext_get_ext(ifp, i);
		if (!isnullstartblock(xfs_bmbt_get_startblock(frp))) {
			(*numrecs)++;
			*count += xfs_bmbt_get_blockcount(frp);
		}
	}
}

/*
 * Count leaf blocks given a range of extent records originally
 * in btree format.
 */
STATIC void
xfs_bmap_disk_count_leaves(
	struct xfs_mount	*mp,
	struct xfs_btree_block	*block,
	int			numrecs,
	xfs_filblks_t		*count)
{
	int		b;
	xfs_bmbt_rec_t	*frp;

	for (b = 1; b <= numrecs; b++) {
		frp = XFS_BMBT_REC_ADDR(mp, block, b);
		*count += xfs_bmbt_disk_get_blockcount(frp);
	}
}

/*
 * Recursively walks each level of a btree
 * to count total fsblocks in use.
 */
STATIC int
xfs_bmap_count_tree(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_ifork	*ifp,
	xfs_fsblock_t		blockno,
	int			levelin,
	xfs_extnum_t		*nextents,
	xfs_filblks_t		*count)
{
	int			error;
	struct xfs_buf		*bp, *nbp;
	int			level = levelin;
	__be64			*pp;
	xfs_fsblock_t           bno = blockno;
	xfs_fsblock_t		nextbno;
	struct xfs_btree_block	*block, *nextblock;
	int			numrecs;

	error = xfs_btree_read_bufl(mp, tp, bno, 0, &bp, XFS_BMAP_BTREE_REF,
						&xfs_bmbt_buf_ops);
	if (error)
		return error;
	*count += 1;
	block = XFS_BUF_TO_BLOCK(bp);

	if (--level) {
		/* Not at node above leaves, count this level of nodes */
		nextbno = be64_to_cpu(block->bb_u.l.bb_rightsib);
		while (nextbno != NULLFSBLOCK) {
			error = xfs_btree_read_bufl(mp, tp, nextbno, 0, &nbp,
						XFS_BMAP_BTREE_REF,
						&xfs_bmbt_buf_ops);
			if (error)
				return error;
			*count += 1;
			nextblock = XFS_BUF_TO_BLOCK(nbp);
			nextbno = be64_to_cpu(nextblock->bb_u.l.bb_rightsib);
			xfs_trans_brelse(tp, nbp);
		}

		/* Dive to the next level */
		pp = XFS_BMBT_PTR_ADDR(mp, block, 1, mp->m_bmap_dmxr[1]);
		bno = be64_to_cpu(*pp);
		error = xfs_bmap_count_tree(mp, tp, ifp, bno, level, nextents,
				count);
		if (error) {
			xfs_trans_brelse(tp, bp);
			XFS_ERROR_REPORT("xfs_bmap_count_tree(1)",
					 XFS_ERRLEVEL_LOW, mp);
			return -EFSCORRUPTED;
		}
		xfs_trans_brelse(tp, bp);
	} else {
		/* count all level 1 nodes and their leaves */
		for (;;) {
			nextbno = be64_to_cpu(block->bb_u.l.bb_rightsib);
			numrecs = be16_to_cpu(block->bb_numrecs);
			(*nextents) += numrecs;
			xfs_bmap_disk_count_leaves(mp, block, numrecs, count);
			xfs_trans_brelse(tp, bp);
			if (nextbno == NULLFSBLOCK)
				break;
			bno = nextbno;
			error = xfs_btree_read_bufl(mp, tp, bno, 0, &bp,
						XFS_BMAP_BTREE_REF,
						&xfs_bmbt_buf_ops);
			if (error)
				return error;
			*count += 1;
			block = XFS_BUF_TO_BLOCK(bp);
		}
	}
	return 0;
}

/*
 * Count fsblocks of the given fork.  Delayed allocation extents are
 * not counted towards the totals.
 */
int
xfs_bmap_count_blocks(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	xfs_extnum_t		*nextents,
	xfs_filblks_t		*count)
{
	struct xfs_mount	*mp;	/* file system mount structure */
	__be64			*pp;	/* pointer to block address */
	struct xfs_btree_block	*block;	/* current btree block */
	struct xfs_ifork	*ifp;	/* fork structure */
	xfs_fsblock_t		bno;	/* block # of "block" */
	int			level;	/* btree level, for checking */
	int			error;

	bno = NULLFSBLOCK;
	mp = ip->i_mount;
	*nextents = 0;
	*count = 0;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!ifp)
		return 0;

	switch (XFS_IFORK_FORMAT(ip, whichfork)) {
	case XFS_DINODE_FMT_EXTENTS:
		xfs_bmap_count_leaves(ifp, nextents, count);
		return 0;
	case XFS_DINODE_FMT_BTREE:
		if (!(ifp->if_flags & XFS_IFEXTENTS)) {
			error = xfs_iread_extents(tp, ip, whichfork);
			if (error)
				return error;
		}

		/*
		 * Root level must use BMAP_BROOT_PTR_ADDR macro to get ptr out.
		 */
		block = ifp->if_broot;
		level = be16_to_cpu(block->bb_level);
		ASSERT(level > 0);
		pp = XFS_BMAP_BROOT_PTR_ADDR(mp, block, 1, ifp->if_broot_bytes);
		bno = be64_to_cpu(*pp);
		ASSERT(bno != NULLFSBLOCK);
		ASSERT(XFS_FSB_TO_AGNO(mp, bno) < mp->m_sb.sb_agcount);
		ASSERT(XFS_FSB_TO_AGBNO(mp, bno) < mp->m_sb.sb_agblocks);

		error = xfs_bmap_count_tree(mp, tp, ifp, bno, level,
				nextents, count);
		if (error) {
			XFS_ERROR_REPORT("xfs_bmap_count_blocks(2)",
					XFS_ERRLEVEL_LOW, mp);
			return -EFSCORRUPTED;
		}
		return 0;
	}

	return 0;
}

/*
 * returns 1 for success, 0 if we failed to map the extent.
 */
STATIC int
xfs_getbmapx_fix_eof_hole(
	xfs_inode_t		*ip,		/* xfs incore inode pointer */
	struct getbmapx		*out,		/* output structure */
	int			prealloced,	/* this is a file with
						 * preallocated data space */
	int64_t			end,		/* last block requested */
	xfs_fsblock_t		startblock)
{
	int64_t			fixlen;
	xfs_mount_t		*mp;		/* file system mount point */
	xfs_ifork_t		*ifp;		/* inode fork pointer */
	xfs_extnum_t		lastx;		/* last extent pointer */
	xfs_fileoff_t		fileblock;

	if (startblock == HOLESTARTBLOCK) {
		mp = ip->i_mount;
		out->bmv_block = -1;
		fixlen = XFS_FSB_TO_BB(mp, XFS_B_TO_FSB(mp, XFS_ISIZE(ip)));
		fixlen -= out->bmv_offset;
		if (prealloced && out->bmv_offset + out->bmv_length == end) {
			/* Came to hole at EOF. Trim it. */
			if (fixlen <= 0)
				return 0;
			out->bmv_length = fixlen;
		}
	} else {
		if (startblock == DELAYSTARTBLOCK)
			out->bmv_block = -2;
		else
			out->bmv_block = xfs_fsb_to_db(ip, startblock);
		fileblock = XFS_BB_TO_FSB(ip->i_mount, out->bmv_offset);
		ifp = XFS_IFORK_PTR(ip, XFS_DATA_FORK);
		if (xfs_iext_bno_to_ext(ifp, fileblock, &lastx) &&
		   (lastx == xfs_iext_count(ifp) - 1))
			out->bmv_oflags |= BMV_OF_LAST;
	}

	return 1;
}

/*
 * Get inode's extents as described in bmv, and format for output.
 * Calls formatter to fill the user's buffer until all extents
 * are mapped, until the passed-in bmv->bmv_count slots have
 * been filled, or until the formatter short-circuits the loop,
 * if it is tracking filled-in extents on its own.
 */
int						/* error code */
xfs_getbmap(
	xfs_inode_t		*ip,
	struct getbmapx		*bmv,		/* user bmap structure */
	xfs_bmap_format_t	formatter,	/* format to user */
	void			*arg)		/* formatter arg */
{
	int64_t			bmvend;		/* last block requested */
	int			error = 0;	/* return value */
	int64_t			fixlen;		/* length for -1 case */
	int			i;		/* extent number */
	int			lock;		/* lock state */
	xfs_bmbt_irec_t		*map;		/* buffer for user's data */
	xfs_mount_t		*mp;		/* file system mount point */
	int			nex;		/* # of user extents can do */
	int			nexleft;	/* # of user extents left */
	int			subnex;		/* # of bmapi's can do */
	int			nmap;		/* number of map entries */
	struct getbmapx		*out;		/* output structure */
	int			whichfork;	/* data or attr fork */
	int			prealloced;	/* this is a file with
						 * preallocated data space */
	int			iflags;		/* interface flags */
	int			bmapi_flags;	/* flags for xfs_bmapi */
	int			cur_ext = 0;

	mp = ip->i_mount;
	iflags = bmv->bmv_iflags;
	whichfork = iflags & BMV_IF_ATTRFORK ? XFS_ATTR_FORK : XFS_DATA_FORK;

	if (whichfork == XFS_ATTR_FORK) {
		if (XFS_IFORK_Q(ip)) {
			if (ip->i_d.di_aformat != XFS_DINODE_FMT_EXTENTS &&
			    ip->i_d.di_aformat != XFS_DINODE_FMT_BTREE &&
			    ip->i_d.di_aformat != XFS_DINODE_FMT_LOCAL)
				return -EINVAL;
		} else if (unlikely(
			   ip->i_d.di_aformat != 0 &&
			   ip->i_d.di_aformat != XFS_DINODE_FMT_EXTENTS)) {
			XFS_ERROR_REPORT("xfs_getbmap", XFS_ERRLEVEL_LOW,
					 ip->i_mount);
			return -EFSCORRUPTED;
		}

		prealloced = 0;
		fixlen = 1LL << 32;
	} else {
		/* Local format data forks report no extents. */
		if (ip->i_d.di_format == XFS_DINODE_FMT_LOCAL) {
			bmv->bmv_entries = 0;
			return 0;
		}
		if (ip->i_d.di_format != XFS_DINODE_FMT_EXTENTS &&
		    ip->i_d.di_format != XFS_DINODE_FMT_BTREE)
			return -EINVAL;

		if (xfs_get_extsz_hint(ip) ||
		    ip->i_d.di_flags & (XFS_DIFLAG_PREALLOC|XFS_DIFLAG_APPEND)){
			prealloced = 1;
			fixlen = mp->m_super->s_maxbytes;
		} else {
			prealloced = 0;
			fixlen = XFS_ISIZE(ip);
		}
	}

	if (bmv->bmv_length == -1) {
		fixlen = XFS_FSB_TO_BB(mp, XFS_B_TO_FSB(mp, fixlen));
		bmv->bmv_length =
			max_t(int64_t, fixlen - bmv->bmv_offset, 0);
	} else if (bmv->bmv_length == 0) {
		bmv->bmv_entries = 0;
		return 0;
	} else if (bmv->bmv_length < 0) {
		return -EINVAL;
	}

	nex = bmv->bmv_count - 1;
	if (nex <= 0)
		return -EINVAL;
	bmvend = bmv->bmv_offset + bmv->bmv_length;


	if (bmv->bmv_count > ULONG_MAX / sizeof(struct getbmapx))
		return -ENOMEM;
	out = kmem_zalloc_large(bmv->bmv_count * sizeof(struct getbmapx), 0);
	if (!out)
		return -ENOMEM;

	xfs_ilock(ip, XFS_IOLOCK_SHARED);
	if (whichfork == XFS_DATA_FORK) {
		if (!(iflags & BMV_IF_DELALLOC) &&
		    (ip->i_delayed_blks || XFS_ISIZE(ip) > ip->i_d.di_size)) {
			error = filemap_write_and_wait(VFS_I(ip)->i_mapping);
			if (error)
				goto out_unlock_iolock;

			/*
			 * Even after flushing the inode, there can still be
			 * delalloc blocks on the inode beyond EOF due to
			 * speculative preallocation.  These are not removed
			 * until the release function is called or the inode
			 * is inactivated.  Hence we cannot assert here that
			 * ip->i_delayed_blks == 0.
			 */
		}

		lock = xfs_ilock_data_map_shared(ip);
	} else {
		lock = xfs_ilock_attr_map_shared(ip);
	}

	/*
	 * Don't let nex be bigger than the number of extents
	 * we can have assuming alternating holes and real extents.
	 */
	if (nex > XFS_IFORK_NEXTENTS(ip, whichfork) * 2 + 1)
		nex = XFS_IFORK_NEXTENTS(ip, whichfork) * 2 + 1;

	bmapi_flags = xfs_bmapi_aflag(whichfork);
	if (!(iflags & BMV_IF_PREALLOC))
		bmapi_flags |= XFS_BMAPI_IGSTATE;

	/*
	 * Allocate enough space to handle "subnex" maps at a time.
	 */
	error = -ENOMEM;
	subnex = 16;
	map = kmem_alloc(subnex * sizeof(*map), KM_MAYFAIL | KM_NOFS);
	if (!map)
		goto out_unlock_ilock;

	bmv->bmv_entries = 0;

	if (XFS_IFORK_NEXTENTS(ip, whichfork) == 0 &&
	    (whichfork == XFS_ATTR_FORK || !(iflags & BMV_IF_DELALLOC))) {
		error = 0;
		goto out_free_map;
	}

	nexleft = nex;

	do {
		nmap = (nexleft > subnex) ? subnex : nexleft;
		error = xfs_bmapi_read(ip, XFS_BB_TO_FSBT(mp, bmv->bmv_offset),
				       XFS_BB_TO_FSB(mp, bmv->bmv_length),
				       map, &nmap, bmapi_flags);
		if (error)
			goto out_free_map;
		ASSERT(nmap <= subnex);

		for (i = 0; i < nmap && nexleft && bmv->bmv_length; i++) {
			out[cur_ext].bmv_oflags = 0;
			if (map[i].br_state == XFS_EXT_UNWRITTEN)
				out[cur_ext].bmv_oflags |= BMV_OF_PREALLOC;
			else if (map[i].br_startblock == DELAYSTARTBLOCK)
				out[cur_ext].bmv_oflags |= BMV_OF_DELALLOC;
			out[cur_ext].bmv_offset =
				XFS_FSB_TO_BB(mp, map[i].br_startoff);
			out[cur_ext].bmv_length =
				XFS_FSB_TO_BB(mp, map[i].br_blockcount);
			out[cur_ext].bmv_unused1 = 0;
			out[cur_ext].bmv_unused2 = 0;

			/*
			 * delayed allocation extents that start beyond EOF can
			 * occur due to speculative EOF allocation when the
			 * delalloc extent is larger than the largest freespace
			 * extent at conversion time. These extents cannot be
			 * converted by data writeback, so can exist here even
			 * if we are not supposed to be finding delalloc
			 * extents.
			 */
			if (map[i].br_startblock == DELAYSTARTBLOCK &&
			    map[i].br_startoff < XFS_B_TO_FSB(mp, XFS_ISIZE(ip)))
				ASSERT((iflags & BMV_IF_DELALLOC) != 0);

                        if (map[i].br_startblock == HOLESTARTBLOCK &&
			    whichfork == XFS_ATTR_FORK) {
				/* came to the end of attribute fork */
				out[cur_ext].bmv_oflags |= BMV_OF_LAST;
				goto out_free_map;
			}

			if (!xfs_getbmapx_fix_eof_hole(ip, &out[cur_ext],
					prealloced, bmvend,
					map[i].br_startblock))
				goto out_free_map;

			bmv->bmv_offset =
				out[cur_ext].bmv_offset +
				out[cur_ext].bmv_length;
			bmv->bmv_length =
				max_t(int64_t, 0, bmvend - bmv->bmv_offset);

			/*
			 * In case we don't want to return the hole,
			 * don't increase cur_ext so that we can reuse
			 * it in the next loop.
			 */
			if ((iflags & BMV_IF_NO_HOLES) &&
			    map[i].br_startblock == HOLESTARTBLOCK) {
				memset(&out[cur_ext], 0, sizeof(out[cur_ext]));
				continue;
			}

			nexleft--;
			bmv->bmv_entries++;
			cur_ext++;
		}
	} while (nmap && nexleft && bmv->bmv_length);

 out_free_map:
	kmem_free(map);
 out_unlock_ilock:
	xfs_iunlock(ip, lock);
 out_unlock_iolock:
	xfs_iunlock(ip, XFS_IOLOCK_SHARED);

	for (i = 0; i < cur_ext; i++) {
		/* format results & advance arg */
		error = formatter(&arg, &out[i]);
		if (error)
			break;
	}

	kmem_free(out);
	return error;
}

/*
 * dead simple method of punching delalyed allocation blocks from a range in
 * the inode. Walks a block at a time so will be slow, but is only executed in
 * rare error cases so the overhead is not critical. This will always punch out
 * both the start and end blocks, even if the ranges only partially overlap
 * them, so it is up to the caller to ensure that partial blocks are not
 * passed in.
 */
int
xfs_bmap_punch_delalloc_range(
	struct xfs_inode	*ip,
	xfs_fileoff_t		start_fsb,
	xfs_fileoff_t		length)
{
	xfs_fileoff_t		remaining = length;
	int			error = 0;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));

	do {
		int		done;
		xfs_bmbt_irec_t	imap;
		int		nimaps = 1;
		xfs_fsblock_t	firstblock;
		struct xfs_defer_ops dfops;

		/*
		 * Map the range first and check that it is a delalloc extent
		 * before trying to unmap the range. Otherwise we will be
		 * trying to remove a real extent (which requires a
		 * transaction) or a hole, which is probably a bad idea...
		 */
		error = xfs_bmapi_read(ip, start_fsb, 1, &imap, &nimaps,
				       XFS_BMAPI_ENTIRE);

		if (error) {
			/* something screwed, just bail */
			if (!XFS_FORCED_SHUTDOWN(ip->i_mount)) {
				xfs_alert(ip->i_mount,
			"Failed delalloc mapping lookup ino %lld fsb %lld.",
						ip->i_ino, start_fsb);
			}
			break;
		}
		if (!nimaps) {
			/* nothing there */
			goto next_block;
		}
		if (imap.br_startblock != DELAYSTARTBLOCK) {
			/* been converted, ignore */
			goto next_block;
		}
		WARN_ON(imap.br_blockcount == 0);

		/*
		 * Note: while we initialise the firstblock/dfops pair, they
		 * should never be used because blocks should never be
		 * allocated or freed for a delalloc extent and hence we need
		 * don't cancel or finish them after the xfs_bunmapi() call.
		 */
		xfs_defer_init(&dfops, &firstblock);
		error = xfs_bunmapi(NULL, ip, start_fsb, 1, 0, 1, &firstblock,
					&dfops, &done);
		if (error)
			break;

		ASSERT(!xfs_defer_has_unfinished_work(&dfops));
next_block:
		start_fsb++;
		remaining--;
	} while(remaining > 0);

	return error;
}

/*
 * Test whether it is appropriate to check an inode for and free post EOF
 * blocks. The 'force' parameter determines whether we should also consider
 * regular files that are marked preallocated or append-only.
 */
bool
xfs_can_free_eofblocks(struct xfs_inode *ip, bool force)
{
	/* prealloc/delalloc exists only on regular files */
	if (!S_ISREG(VFS_I(ip)->i_mode))
		return false;

	/*
	 * Zero sized files with no cached pages and delalloc blocks will not
	 * have speculative prealloc/delalloc blocks to remove.
	 */
	if (VFS_I(ip)->i_size == 0 &&
	    VFS_I(ip)->i_mapping->nrpages == 0 &&
	    ip->i_delayed_blks == 0)
		return false;

	/* If we haven't read in the extent list, then don't do it now. */
	if (!(ip->i_df.if_flags & XFS_IFEXTENTS))
		return false;

	/*
	 * Do not free real preallocated or append-only files unless the file
	 * has delalloc blocks and we are forced to remove them.
	 */
	if (ip->i_d.di_flags & (XFS_DIFLAG_PREALLOC | XFS_DIFLAG_APPEND))
		if (!force || ip->i_delayed_blks == 0)
			return false;

	return true;
}

/*
 * This is called to free any blocks beyond eof. The caller must hold
 * IOLOCK_EXCL unless we are in the inode reclaim path and have the only
 * reference to the inode.
 */
int
xfs_free_eofblocks(
	struct xfs_inode	*ip)
{
	struct xfs_trans	*tp;
	int			error;
	xfs_fileoff_t		end_fsb;
	xfs_fileoff_t		last_fsb;
	xfs_filblks_t		map_len;
	int			nimaps;
	struct xfs_bmbt_irec	imap;
	struct xfs_mount	*mp = ip->i_mount;

	/*
	 * Figure out if there are any blocks beyond the end
	 * of the file.  If not, then there is nothing to do.
	 */
	end_fsb = XFS_B_TO_FSB(mp, (xfs_ufsize_t)XFS_ISIZE(ip));
	last_fsb = XFS_B_TO_FSB(mp, mp->m_super->s_maxbytes);
	if (last_fsb <= end_fsb)
		return 0;
	map_len = last_fsb - end_fsb;

	nimaps = 1;
	xfs_ilock(ip, XFS_ILOCK_SHARED);
	error = xfs_bmapi_read(ip, end_fsb, map_len, &imap, &nimaps, 0);
	xfs_iunlock(ip, XFS_ILOCK_SHARED);

	/*
	 * If there are blocks after the end of file, truncate the file to its
	 * current size to free them up.
	 */
	if (!error && (nimaps != 0) &&
	    (imap.br_startblock != HOLESTARTBLOCK ||
	     ip->i_delayed_blks)) {
		/*
		 * Attach the dquots to the inode up front.
		 */
		error = xfs_qm_dqattach(ip, 0);
		if (error)
			return error;

		/* wait on dio to ensure i_size has settled */
		inode_dio_wait(VFS_I(ip));

		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0,
				&tp);
		if (error) {
			ASSERT(XFS_FORCED_SHUTDOWN(mp));
			return error;
		}

		xfs_ilock(ip, XFS_ILOCK_EXCL);
		xfs_trans_ijoin(tp, ip, 0);

		/*
		 * Do not update the on-disk file size.  If we update the
		 * on-disk file size and then the system crashes before the
		 * contents of the file are flushed to disk then the files
		 * may be full of holes (ie NULL files bug).
		 */
		error = xfs_itruncate_extents(&tp, ip, XFS_DATA_FORK,
					      XFS_ISIZE(ip));
		if (error) {
			/*
			 * If we get an error at this point we simply don't
			 * bother truncating the file.
			 */
			xfs_trans_cancel(tp);
		} else {
			error = xfs_trans_commit(tp);
			if (!error)
				xfs_inode_clear_eofblocks_tag(ip);
		}

		xfs_iunlock(ip, XFS_ILOCK_EXCL);
	}
	return error;
}

int
xfs_alloc_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len,
	int			alloc_type)
{
	xfs_mount_t		*mp = ip->i_mount;
	xfs_off_t		count;
	xfs_filblks_t		allocated_fsb;
	xfs_filblks_t		allocatesize_fsb;
	xfs_extlen_t		extsz, temp;
	xfs_fileoff_t		startoffset_fsb;
	xfs_fsblock_t		firstfsb;
	int			nimaps;
	int			quota_flag;
	int			rt;
	xfs_trans_t		*tp;
	xfs_bmbt_irec_t		imaps[1], *imapp;
	struct xfs_defer_ops	dfops;
	uint			qblocks, resblks, resrtextents;
	int			error;

	trace_xfs_alloc_file_space(ip);

	if (XFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	error = xfs_qm_dqattach(ip, 0);
	if (error)
		return error;

	if (len <= 0)
		return -EINVAL;

	rt = XFS_IS_REALTIME_INODE(ip);
	extsz = xfs_get_extsz_hint(ip);

	count = len;
	imapp = &imaps[0];
	nimaps = 1;
	startoffset_fsb	= XFS_B_TO_FSBT(mp, offset);
	allocatesize_fsb = XFS_B_TO_FSB(mp, count);

	/*
	 * Allocate file space until done or until there is an error
	 */
	while (allocatesize_fsb && !error) {
		xfs_fileoff_t	s, e;

		/*
		 * Determine space reservations for data/realtime.
		 */
		if (unlikely(extsz)) {
			s = startoffset_fsb;
			do_div(s, extsz);
			s *= extsz;
			e = startoffset_fsb + allocatesize_fsb;
			if ((temp = do_mod(startoffset_fsb, extsz)))
				e += temp;
			if ((temp = do_mod(e, extsz)))
				e += extsz - temp;
		} else {
			s = 0;
			e = allocatesize_fsb;
		}

		/*
		 * The transaction reservation is limited to a 32-bit block
		 * count, hence we need to limit the number of blocks we are
		 * trying to reserve to avoid an overflow. We can't allocate
		 * more than @nimaps extents, and an extent is limited on disk
		 * to MAXEXTLEN (21 bits), so use that to enforce the limit.
		 */
		resblks = min_t(xfs_fileoff_t, (e - s), (MAXEXTLEN * nimaps));
		if (unlikely(rt)) {
			resrtextents = qblocks = resblks;
			resrtextents /= mp->m_sb.sb_rextsize;
			resblks = XFS_DIOSTRAT_SPACE_RES(mp, 0);
			quota_flag = XFS_QMOPT_RES_RTBLKS;
		} else {
			resrtextents = 0;
			resblks = qblocks = XFS_DIOSTRAT_SPACE_RES(mp, resblks);
			quota_flag = XFS_QMOPT_RES_REGBLKS;
		}

		/*
		 * Allocate and setup the transaction.
		 */
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks,
				resrtextents, 0, &tp);

		/*
		 * Check for running out of space
		 */
		if (error) {
			/*
			 * Free the transaction structure.
			 */
			ASSERT(error == -ENOSPC || XFS_FORCED_SHUTDOWN(mp));
			break;
		}
		xfs_ilock(ip, XFS_ILOCK_EXCL);
		error = xfs_trans_reserve_quota_nblks(tp, ip, qblocks,
						      0, quota_flag);
		if (error)
			goto error1;

		xfs_trans_ijoin(tp, ip, 0);

		xfs_defer_init(&dfops, &firstfsb);
		error = xfs_bmapi_write(tp, ip, startoffset_fsb,
					allocatesize_fsb, alloc_type, &firstfsb,
					resblks, imapp, &nimaps, &dfops);
		if (error)
			goto error0;

		/*
		 * Complete the transaction
		 */
		error = xfs_defer_finish(&tp, &dfops, NULL);
		if (error)
			goto error0;

		error = xfs_trans_commit(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			break;

		allocated_fsb = imapp->br_blockcount;

		if (nimaps == 0) {
			error = -ENOSPC;
			break;
		}

		startoffset_fsb += allocated_fsb;
		allocatesize_fsb -= allocated_fsb;
	}

	return error;

error0:	/* Cancel bmap, unlock inode, unreserve quota blocks, cancel trans */
	xfs_defer_cancel(&dfops);
	xfs_trans_unreserve_quota_nblks(tp, ip, (long)qblocks, 0, quota_flag);

error1:	/* Just cancel transaction */
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

static int
xfs_unmap_extent(
	struct xfs_inode	*ip,
	xfs_fileoff_t		startoffset_fsb,
	xfs_filblks_t		len_fsb,
	int			*done)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	struct xfs_defer_ops	dfops;
	xfs_fsblock_t		firstfsb;
	uint			resblks = XFS_DIOSTRAT_SPACE_RES(mp, 0);
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
	if (error) {
		ASSERT(error == -ENOSPC || XFS_FORCED_SHUTDOWN(mp));
		return error;
	}

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	error = xfs_trans_reserve_quota(tp, mp, ip->i_udquot, ip->i_gdquot,
			ip->i_pdquot, resblks, 0, XFS_QMOPT_RES_REGBLKS);
	if (error)
		goto out_trans_cancel;

	xfs_trans_ijoin(tp, ip, 0);

	xfs_defer_init(&dfops, &firstfsb);
	error = xfs_bunmapi(tp, ip, startoffset_fsb, len_fsb, 0, 2, &firstfsb,
			&dfops, done);
	if (error)
		goto out_bmap_cancel;

	error = xfs_defer_finish(&tp, &dfops, ip);
	if (error)
		goto out_bmap_cancel;

	error = xfs_trans_commit(tp);
out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

out_bmap_cancel:
	xfs_defer_cancel(&dfops);
out_trans_cancel:
	xfs_trans_cancel(tp);
	goto out_unlock;
}

static int
xfs_adjust_extent_unmap_boundaries(
	struct xfs_inode	*ip,
	xfs_fileoff_t		*startoffset_fsb,
	xfs_fileoff_t		*endoffset_fsb)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_bmbt_irec	imap;
	int			nimap, error;
	xfs_extlen_t		mod = 0;

	nimap = 1;
	error = xfs_bmapi_read(ip, *startoffset_fsb, 1, &imap, &nimap, 0);
	if (error)
		return error;

	if (nimap && imap.br_startblock != HOLESTARTBLOCK) {
		ASSERT(imap.br_startblock != DELAYSTARTBLOCK);
		mod = do_mod(imap.br_startblock, mp->m_sb.sb_rextsize);
		if (mod)
			*startoffset_fsb += mp->m_sb.sb_rextsize - mod;
	}

	nimap = 1;
	error = xfs_bmapi_read(ip, *endoffset_fsb - 1, 1, &imap, &nimap, 0);
	if (error)
		return error;

	if (nimap && imap.br_startblock != HOLESTARTBLOCK) {
		ASSERT(imap.br_startblock != DELAYSTARTBLOCK);
		mod++;
		if (mod && mod != mp->m_sb.sb_rextsize)
			*endoffset_fsb -= mod;
	}

	return 0;
}

static int
xfs_flush_unmap_range(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct inode		*inode = VFS_I(ip);
	xfs_off_t		rounding, start, end;
	int			error;

	/* wait for the completion of any pending DIOs */
	inode_dio_wait(inode);

	rounding = max_t(xfs_off_t, 1 << mp->m_sb.sb_blocklog, PAGE_SIZE);
	start = round_down(offset, rounding);
	end = round_up(offset + len, rounding) - 1;

	error = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (error)
		return error;
	truncate_pagecache_range(inode, start, end);
	return 0;
}

int
xfs_free_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		startoffset_fsb;
	xfs_fileoff_t		endoffset_fsb;
	int			done = 0, error;

	trace_xfs_free_file_space(ip);

	error = xfs_qm_dqattach(ip, 0);
	if (error)
		return error;

	if (len <= 0)	/* if nothing being freed */
		return 0;

	error = xfs_flush_unmap_range(ip, offset, len);
	if (error)
		return error;

	startoffset_fsb = XFS_B_TO_FSB(mp, offset);
	endoffset_fsb = XFS_B_TO_FSBT(mp, offset + len);

	/*
	 * Need to zero the stuff we're not freeing, on disk.  If it's a RT file
	 * and we can't use unwritten extents then we actually need to ensure
	 * to zero the whole extent, otherwise we just need to take of block
	 * boundaries, and xfs_bunmapi will handle the rest.
	 */
	if (XFS_IS_REALTIME_INODE(ip) &&
	    !xfs_sb_version_hasextflgbit(&mp->m_sb)) {
		error = xfs_adjust_extent_unmap_boundaries(ip, &startoffset_fsb,
				&endoffset_fsb);
		if (error)
			return error;
	}

	if (endoffset_fsb > startoffset_fsb) {
		while (!done) {
			error = xfs_unmap_extent(ip, startoffset_fsb,
					endoffset_fsb - startoffset_fsb, &done);
			if (error)
				return error;
		}
	}

	/*
	 * Now that we've unmap all full blocks we'll have to zero out any
	 * partial block at the beginning and/or end.  xfs_zero_range is
	 * smart enough to skip any holes, including those we just created,
	 * but we must take care not to zero beyond EOF and enlarge i_size.
	 */

	if (offset >= XFS_ISIZE(ip))
		return 0;

	if (offset + len > XFS_ISIZE(ip))
		len = XFS_ISIZE(ip) - offset;

	return xfs_zero_range(ip, offset, len, NULL);
}

/*
 * Preallocate and zero a range of a file. This mechanism has the allocation
 * semantics of fallocate and in addition converts data in the range to zeroes.
 */
int
xfs_zero_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	struct xfs_mount	*mp = ip->i_mount;
	uint			blksize;
	int			error;

	trace_xfs_zero_file_space(ip);

	blksize = 1 << mp->m_sb.sb_blocklog;

	/*
	 * Punch a hole and prealloc the range. We use hole punch rather than
	 * unwritten extent conversion for two reasons:
	 *
	 * 1.) Hole punch handles partial block zeroing for us.
	 *
	 * 2.) If prealloc returns ENOSPC, the file range is still zero-valued
	 * by virtue of the hole punch.
	 */
	error = xfs_free_file_space(ip, offset, len);
	if (error)
		goto out;

	error = xfs_alloc_file_space(ip, round_down(offset, blksize),
				     round_up(offset + len, blksize) -
				     round_down(offset, blksize),
				     XFS_BMAPI_PREALLOC);
out:
	return error;

}

/*
 * xfs_collapse_file_space()
 *	This routine frees disk space and shift extent for the given file.
 *	The first thing we do is to free data blocks in the specified range
 *	by calling xfs_free_file_space(). It would also sync dirty data
 *	and invalidate page cache over the region on which collapse range
 *	is working. And Shift extent records to the left to cover a hole.
 * RETURNS:
 *	0 on success
 *	errno on error
 *
 */
int
xfs_collapse_file_space(
	struct xfs_inode	*ip,
	xfs_off_t		offset,
	xfs_off_t		len)
{
	int			done = 0;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error;
	struct xfs_defer_ops	dfops;
	xfs_fsblock_t		first_block;
	xfs_fileoff_t		start_fsb;
	xfs_fileoff_t		next_fsb;
	xfs_fileoff_t		shift_fsb;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));

	trace_xfs_collapse_file_space(ip);

	next_fsb = XFS_B_TO_FSB(mp, offset + len);
	shift_fsb = XFS_B_TO_FSB(mp, len);

	error = xfs_free_file_space(ip, offset, len);
	if (error)
		return error;

	/*
	 * Trim eofblocks to avoid shifting uninitialized post-eof preallocation
	 * into the accessible region of the file.
	 */
	if (xfs_can_free_eofblocks(ip, true)) {
		error = xfs_free_eofblocks(ip);
		if (error)
			return error;
	}

	/*
	 * Writeback and invalidate cache for the remainder of the file as we're
	 * about to shift down every extent from the collapse range to EOF. The
	 * free of the collapse range above might have already done some of
	 * this, but we shouldn't rely on it to do anything outside of the range
	 * that was freed.
	 */
	error = filemap_write_and_wait_range(VFS_I(ip)->i_mapping,
					     offset + len, -1);
	if (error)
		return error;
	error = invalidate_inode_pages2_range(VFS_I(ip)->i_mapping,
					(offset + len) >> PAGE_CACHE_SHIFT, -1);
	if (error)
		return error;

	while (!error && !done) {
		/*
		 * We would need to reserve permanent block for transaction.
		 * This will come into picture when after shifting extent into
		 * hole we found that adjacent extents can be merged which
		 * may lead to freeing of a block during record update.
		 */
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write,
				XFS_DIOSTRAT_SPACE_RES(mp, 0), 0, 0, &tp);
		if (error)
			break;

		xfs_ilock(ip, XFS_ILOCK_EXCL);
		error = xfs_trans_reserve_quota(tp, mp, ip->i_udquot,
				ip->i_gdquot, ip->i_pdquot,
				XFS_DIOSTRAT_SPACE_RES(mp, 0), 0,
				XFS_QMOPT_RES_REGBLKS);
		if (error)
			goto out_trans_cancel;

		xfs_trans_ijoin(tp, ip, 0);

		xfs_defer_init(&dfops, &first_block);

		/*
		 * We are using the write transaction in which max 2 bmbt
		 * updates are allowed
		 */
		start_fsb = next_fsb;
		error = xfs_bmap_shift_extents(tp, ip, start_fsb, shift_fsb,
				&done, &next_fsb, &first_block, &dfops,
				XFS_BMAP_MAX_SHIFT_EXTENTS);
		if (error)
			goto out_bmap_cancel;

		error = xfs_defer_finish(&tp, &dfops, NULL);
		if (error)
			goto out_bmap_cancel;

		error = xfs_trans_commit(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
	}

	return error;

out_bmap_cancel:
	xfs_defer_cancel(&dfops);
out_trans_cancel:
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * We need to check that the format of the data fork in the temporary inode is
 * valid for the target inode before doing the swap. This is not a problem with
 * attr1 because of the fixed fork offset, but attr2 has a dynamically sized
 * data fork depending on the space the attribute fork is taking so we can get
 * invalid formats on the target inode.
 *
 * E.g. target has space for 7 extents in extent format, temp inode only has
 * space for 6.  If we defragment down to 7 extents, then the tmp format is a
 * btree, but when swapped it needs to be in extent format. Hence we can't just
 * blindly swap data forks on attr2 filesystems.
 *
 * Note that we check the swap in both directions so that we don't end up with
 * a corrupt temporary inode, either.
 *
 * Note that fixing the way xfs_fsr sets up the attribute fork in the source
 * inode will prevent this situation from occurring, so all we do here is
 * reject and log the attempt. basically we are putting the responsibility on
 * userspace to get this right.
 */
static int
xfs_swap_extents_check_format(
	struct xfs_inode	*ip,	/* target inode */
	struct xfs_inode	*tip)	/* tmp inode */
{

	/* Should never get a local format */
	if (ip->i_d.di_format == XFS_DINODE_FMT_LOCAL ||
	    tip->i_d.di_format == XFS_DINODE_FMT_LOCAL)
		return -EINVAL;

	/*
	 * if the target inode has less extents that then temporary inode then
	 * why did userspace call us?
	 */
	if (ip->i_d.di_nextents < tip->i_d.di_nextents)
		return -EINVAL;

	/*
	 * if the target inode is in extent form and the temp inode is in btree
	 * form then we will end up with the target inode in the wrong format
	 * as we already know there are less extents in the temp inode.
	 */
	if (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
	    tip->i_d.di_format == XFS_DINODE_FMT_BTREE)
		return -EINVAL;

	/* Check temp in extent form to max in target */
	if (tip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_NEXTENTS(tip, XFS_DATA_FORK) >
			XFS_IFORK_MAXEXT(ip, XFS_DATA_FORK))
		return -EINVAL;

	/* Check target in extent form to max in temp */
	if (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_NEXTENTS(ip, XFS_DATA_FORK) >
			XFS_IFORK_MAXEXT(tip, XFS_DATA_FORK))
		return -EINVAL;

	/*
	 * If we are in a btree format, check that the temp root block will fit
	 * in the target and that it has enough extents to be in btree format
	 * in the target.
	 *
	 * Note that we have to be careful to allow btree->extent conversions
	 * (a common defrag case) which will occur when the temp inode is in
	 * extent format...
	 */
	if (tip->i_d.di_format == XFS_DINODE_FMT_BTREE) {
		if (XFS_IFORK_Q(ip) &&
		    XFS_BMAP_BMDR_SPACE(tip->i_df.if_broot) > XFS_IFORK_BOFF(ip))
			return -EINVAL;
		if (XFS_IFORK_NEXTENTS(tip, XFS_DATA_FORK) <=
		    XFS_IFORK_MAXEXT(ip, XFS_DATA_FORK))
			return -EINVAL;
	}

	/* Reciprocal target->temp btree format checks */
	if (ip->i_d.di_format == XFS_DINODE_FMT_BTREE) {
		if (XFS_IFORK_Q(tip) &&
		    XFS_BMAP_BMDR_SPACE(ip->i_df.if_broot) > XFS_IFORK_BOFF(tip))
			return -EINVAL;
		if (XFS_IFORK_NEXTENTS(ip, XFS_DATA_FORK) <=
		    XFS_IFORK_MAXEXT(tip, XFS_DATA_FORK))
			return -EINVAL;
	}

	return 0;
}

static int
xfs_swap_extent_flush(
	struct xfs_inode	*ip)
{
	int	error;

	error = filemap_write_and_wait(VFS_I(ip)->i_mapping);
	if (error)
		return error;
	truncate_pagecache_range(VFS_I(ip), 0, -1);

	/* Verify O_DIRECT for ftmp */
	if (VFS_I(ip)->i_mapping->nrpages)
		return -EINVAL;
	return 0;
}

/* Swap the extents of two files by swapping data forks. */
STATIC int
xfs_swap_extent_forks(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	struct xfs_inode	*tip,
	int			*src_log_flags,
	int			*target_log_flags)
{
	xfs_extnum_t		nextents;
	struct xfs_ifork	tempifp, *ifp, *tifp;
	xfs_filblks_t		aforkblks = 0;
	xfs_filblks_t		taforkblks = 0;
	xfs_extnum_t		junk;
	uint64_t		tmp;
	int			error;

	/*
	 * Count the number of extended attribute blocks
	 */
	if ( ((XFS_IFORK_Q(ip) != 0) && (ip->i_d.di_anextents > 0)) &&
	     (ip->i_d.di_aformat != XFS_DINODE_FMT_LOCAL)) {
		error = xfs_bmap_count_blocks(tp, ip, XFS_ATTR_FORK, &junk,
				&aforkblks);
		if (error)
			return error;
	}
	if ( ((XFS_IFORK_Q(tip) != 0) && (tip->i_d.di_anextents > 0)) &&
	     (tip->i_d.di_aformat != XFS_DINODE_FMT_LOCAL)) {
		error = xfs_bmap_count_blocks(tp, tip, XFS_ATTR_FORK, &junk,
				&taforkblks);
		if (error)
			return error;
	}

	/*
	 * Before we've swapped the forks, lets set the owners of the forks
	 * appropriately. We have to do this as we are demand paging the btree
	 * buffers, and so the validation done on read will expect the owner
	 * field to be correctly set. Once we change the owners, we can swap the
	 * inode forks.
	 */
	if (ip->i_d.di_version == 3 &&
	    ip->i_d.di_format == XFS_DINODE_FMT_BTREE) {
		(*target_log_flags) |= XFS_ILOG_DOWNER;
		error = xfs_bmbt_change_owner(tp, ip, XFS_DATA_FORK,
					      tip->i_ino, NULL);
		if (error)
			return error;
	}

	if (tip->i_d.di_version == 3 &&
	    tip->i_d.di_format == XFS_DINODE_FMT_BTREE) {
		(*src_log_flags) |= XFS_ILOG_DOWNER;
		error = xfs_bmbt_change_owner(tp, tip, XFS_DATA_FORK,
					      ip->i_ino, NULL);
		if (error)
			return error;
	}

	/*
	 * Swap the data forks of the inodes
	 */
	ifp = &ip->i_df;
	tifp = &tip->i_df;
	tempifp = *ifp;		/* struct copy */
	*ifp = *tifp;		/* struct copy */
	*tifp = tempifp;	/* struct copy */

	/*
	 * Fix the on-disk inode values
	 */
	tmp = (uint64_t)ip->i_d.di_nblocks;
	ip->i_d.di_nblocks = tip->i_d.di_nblocks - taforkblks + aforkblks;
	tip->i_d.di_nblocks = tmp + taforkblks - aforkblks;

	tmp = (uint64_t) ip->i_d.di_nextents;
	ip->i_d.di_nextents = tip->i_d.di_nextents;
	tip->i_d.di_nextents = tmp;

	tmp = (uint64_t) ip->i_d.di_format;
	ip->i_d.di_format = tip->i_d.di_format;
	tip->i_d.di_format = tmp;

	/*
	 * The extents in the source inode could still contain speculative
	 * preallocation beyond EOF (e.g. the file is open but not modified
	 * while defrag is in progress). In that case, we need to copy over the
	 * number of delalloc blocks the data fork in the source inode is
	 * tracking beyond EOF so that when the fork is truncated away when the
	 * temporary inode is unlinked we don't underrun the i_delayed_blks
	 * counter on that inode.
	 */
	ASSERT(tip->i_delayed_blks == 0);
	tip->i_delayed_blks = ip->i_delayed_blks;
	ip->i_delayed_blks = 0;

	switch (ip->i_d.di_format) {
	case XFS_DINODE_FMT_EXTENTS:
		/*
		 * If the extents fit in the inode, fix the pointer.  Otherwise
		 * it's already NULL or pointing to the extent.
		 */
		nextents = xfs_iext_count(&ip->i_df);
		if (nextents <= XFS_INLINE_EXTS)
			ifp->if_u1.if_extents = ifp->if_u2.if_inline_ext;
		(*src_log_flags) |= XFS_ILOG_DEXT;
		break;
	case XFS_DINODE_FMT_BTREE:
		ASSERT(ip->i_d.di_version < 3 ||
		       (*src_log_flags & XFS_ILOG_DOWNER));
		(*src_log_flags) |= XFS_ILOG_DBROOT;
		break;
	}

	switch (tip->i_d.di_format) {
	case XFS_DINODE_FMT_EXTENTS:
		/*
		 * If the extents fit in the inode, fix the pointer.  Otherwise
		 * it's already NULL or pointing to the extent.
		 */
		nextents = xfs_iext_count(&tip->i_df);
		if (nextents <= XFS_INLINE_EXTS)
			tifp->if_u1.if_extents = tifp->if_u2.if_inline_ext;
		(*target_log_flags) |= XFS_ILOG_DEXT;
		break;
	case XFS_DINODE_FMT_BTREE:
		(*target_log_flags) |= XFS_ILOG_DBROOT;
		ASSERT(tip->i_d.di_version < 3 ||
		       (*target_log_flags & XFS_ILOG_DOWNER));
		break;
	}

	return 0;
}

int
xfs_swap_extents(
	struct xfs_inode	*ip,	/* target inode */
	struct xfs_inode	*tip,	/* tmp inode */
	struct xfs_swapext	*sxp)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	struct xfs_bstat	*sbp = &sxp->sx_stat;
	int			src_log_flags, target_log_flags;
	int			error = 0;
	int			lock_flags;

	/*
	 * Lock the inodes against other IO, page faults and truncate to
	 * begin with.  Then we can ensure the inodes are flushed and have no
	 * page cache safely. Once we have done this we can take the ilocks and
	 * do the rest of the checks.
	 */
	lock_flags = XFS_IOLOCK_EXCL | XFS_MMAPLOCK_EXCL;
	xfs_lock_two_inodes(ip, tip, XFS_IOLOCK_EXCL);
	xfs_lock_two_inodes(ip, tip, XFS_MMAPLOCK_EXCL);

	/* Verify that both files have the same format */
	if ((VFS_I(ip)->i_mode & S_IFMT) != (VFS_I(tip)->i_mode & S_IFMT)) {
		error = -EINVAL;
		goto out_unlock;
	}

	/* Verify both files are either real-time or non-realtime */
	if (XFS_IS_REALTIME_INODE(ip) != XFS_IS_REALTIME_INODE(tip)) {
		error = -EINVAL;
		goto out_unlock;
	}

	error = xfs_swap_extent_flush(ip);
	if (error)
		goto out_unlock;
	error = xfs_swap_extent_flush(tip);
	if (error)
		goto out_unlock;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_ichange, 0, 0, 0, &tp);
	if (error)
		goto out_unlock;

	/*
	 * Lock and join the inodes to the tansaction so that transaction commit
	 * or cancel will unlock the inodes from this point onwards.
	 */
	xfs_lock_two_inodes(ip, tip, XFS_ILOCK_EXCL);
	lock_flags |= XFS_ILOCK_EXCL;
	xfs_trans_ijoin(tp, ip, 0);
	xfs_trans_ijoin(tp, tip, 0);


	/* Verify all data are being swapped */
	if (sxp->sx_offset != 0 ||
	    sxp->sx_length != ip->i_d.di_size ||
	    sxp->sx_length != tip->i_d.di_size) {
		error = -EFAULT;
		goto out_trans_cancel;
	}

	trace_xfs_swap_extent_before(ip, 0);
	trace_xfs_swap_extent_before(tip, 1);

	/* check inode formats now that data is flushed */
	error = xfs_swap_extents_check_format(ip, tip);
	if (error) {
		xfs_notice(mp,
		    "%s: inode 0x%llx format is incompatible for exchanging.",
				__func__, ip->i_ino);
		goto out_trans_cancel;
	}

	/*
	 * Compare the current change & modify times with that
	 * passed in.  If they differ, we abort this swap.
	 * This is the mechanism used to ensure the calling
	 * process that the file was not changed out from
	 * under it.
	 */
	if ((sbp->bs_ctime.tv_sec != VFS_I(ip)->i_ctime.tv_sec) ||
	    (sbp->bs_ctime.tv_nsec != VFS_I(ip)->i_ctime.tv_nsec) ||
	    (sbp->bs_mtime.tv_sec != VFS_I(ip)->i_mtime.tv_sec) ||
	    (sbp->bs_mtime.tv_nsec != VFS_I(ip)->i_mtime.tv_nsec)) {
		error = -EBUSY;
		goto out_trans_cancel;
	}

	/*
	 * Note the trickiness in setting the log flags - we set the owner log
	 * flag on the opposite inode (i.e. the inode we are setting the new
	 * owner to be) because once we swap the forks and log that, log
	 * recovery is going to see the fork as owned by the swapped inode,
	 * not the pre-swapped inodes.
	 */
	src_log_flags = XFS_ILOG_CORE;
	target_log_flags = XFS_ILOG_CORE;

	error = xfs_swap_extent_forks(tp, ip, tip, &src_log_flags,
			&target_log_flags);
	if (error)
		goto out_trans_cancel;


	xfs_trans_log_inode(tp, ip,  src_log_flags);
	xfs_trans_log_inode(tp, tip, target_log_flags);

	/*
	 * If this is a synchronous mount, make sure that the
	 * transaction goes to disk before returning to the user.
	 */
	if (mp->m_flags & XFS_MOUNT_WSYNC)
		xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);

	trace_xfs_swap_extent_after(ip, 0);
	trace_xfs_swap_extent_after(tip, 1);

	xfs_iunlock(ip, lock_flags);
	xfs_iunlock(tip, lock_flags);
	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);

out_unlock:
	xfs_iunlock(ip, lock_flags);
	xfs_iunlock(tip, lock_flags);
	return error;
}
