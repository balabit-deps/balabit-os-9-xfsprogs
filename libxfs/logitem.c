// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_trans.h"

kmem_zone_t	*xfs_buf_item_zone;
kmem_zone_t	*xfs_ili_zone;		/* inode log item zone */

/*
 * Following functions from fs/xfs/xfs_trans_buf.c
 */

/*
 * Check to see if a buffer matching the given parameters is already
 * a part of the given transaction.
 */
struct xfs_buf *
xfs_trans_buf_item_match(
	xfs_trans_t		*tp,
	struct xfs_buftarg	*btp,
	struct xfs_buf_map	*map,
	int			nmaps)
{
	struct xfs_log_item	*lip;
	struct xfs_buf_log_item *blip;
	int			len = 0;
	int			i;

	for (i = 0; i < nmaps; i++)
		len += map[i].bm_len;

	list_for_each_entry(lip, &tp->t_items, li_trans) {
		blip = (struct xfs_buf_log_item *)lip;
		if (blip->bli_item.li_type == XFS_LI_BUF &&
		    blip->bli_buf->b_target->bt_bdev == btp->bt_bdev &&
		    XFS_BUF_ADDR(blip->bli_buf) == map[0].bm_bn &&
		    blip->bli_buf->b_length == len) {
			ASSERT(blip->bli_buf->b_map_count == nmaps);
			return blip->bli_buf;
		}
	}

	return NULL;
}
/*
 * The following are from fs/xfs/xfs_buf_item.c
 */

/*
 * Allocate a new buf log item to go with the given buffer.
 * Set the buffer's b_log_item field to point to the new
 * buf log item.  If there are other item's attached to the
 * buffer (see xfs_buf_attach_iodone() below), then put the
 * buf log item at the front.
 */
void
xfs_buf_item_init(
	struct xfs_buf		*bp,
	xfs_mount_t		*mp)
{
	xfs_log_item_t		*lip;
	xfs_buf_log_item_t	*bip;

#ifdef LI_DEBUG
	fprintf(stderr, "buf_item_init for buffer %p\n", bp);
#endif

	/*
	 * Check to see if there is already a buf log item for
	 * this buffer.	 If there is, it is guaranteed to be
	 * the first.  If we do already have one, there is
	 * nothing to do here so return.
	 */
	if (bp->b_log_item != NULL) {
		lip = bp->b_log_item;
		if (lip->li_type == XFS_LI_BUF) {
#ifdef LI_DEBUG
			fprintf(stderr,
				"reused buf item %p for pre-logged buffer %p\n",
				lip, bp);
#endif
			return;
		}
	}

	bip = kmem_cache_zalloc(xfs_buf_item_zone, 0);
#ifdef LI_DEBUG
	fprintf(stderr, "adding buf item %p for not-logged buffer %p\n",
		bip, bp);
#endif
	xfs_log_item_init(mp, &bip->bli_item, XFS_LI_BUF);
	bip->bli_buf = bp;
	bip->__bli_format.blf_type = XFS_LI_BUF;
	bip->__bli_format.blf_blkno = (int64_t)XFS_BUF_ADDR(bp);
	bip->__bli_format.blf_len = (unsigned short)bp->b_length;
	bp->b_log_item = bip;
}


/*
 * Mark bytes first through last inclusive as dirty in the buf
 * item's bitmap.
 */
void
xfs_buf_item_log(
	xfs_buf_log_item_t	*bip,
	uint			first,
	uint			last)
{
	/*
	 * Mark the item as having some dirty data for
	 * quick reference in xfs_buf_item_dirty.
	 */
	bip->bli_flags |= XFS_BLI_DIRTY;
}

/*
 * Initialize the inode log item for a newly allocated (in-core) inode.
 */
void
xfs_inode_item_init(
	xfs_inode_t			*ip,
	xfs_mount_t			*mp)
{
	struct xfs_inode_log_item	*iip;

	ASSERT(ip->i_itemp == NULL);
	iip = ip->i_itemp = kmem_cache_zalloc(xfs_ili_zone, 0);
#ifdef LI_DEBUG
	fprintf(stderr, "inode_item_init for inode %llu, iip=%p\n",
		ip->i_ino, iip);
#endif

	xfs_log_item_init(mp, &iip->ili_item, XFS_LI_INODE);
	iip->ili_inode = ip;
}
