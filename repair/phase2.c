// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libxlog.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "protos.h"
#include "err_protos.h"
#include "incore.h"
#include "progress.h"
#include "scan.h"

/* workaround craziness in the xlog routines */
int xlog_recover_do_trans(struct xlog *log, struct xlog_recover *t, int p)
{
	return 0;
}

static void
zero_log(
	struct xfs_mount	*mp)
{
	int			error;
	xfs_daddr_t		head_blk;
	xfs_daddr_t		tail_blk;
	struct xlog		*log = mp->m_log;

	memset(log, 0, sizeof(struct xlog));
	x.logBBsize = XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
	x.logBBstart = XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart);
	x.lbsize = BBSIZE;
	if (xfs_sb_version_hassector(&mp->m_sb))
		x.lbsize <<= (mp->m_sb.sb_logsectlog - BBSHIFT);

	log->l_dev = mp->m_logdev_targp;
	log->l_logBBsize = x.logBBsize;
	log->l_logBBstart = x.logBBstart;
	log->l_sectBBsize  = BTOBB(x.lbsize);
	log->l_mp = mp;
	if (xfs_sb_version_hassector(&mp->m_sb)) {
		log->l_sectbb_log = mp->m_sb.sb_logsectlog - BBSHIFT;
		ASSERT(log->l_sectbb_log <= mp->m_sectbb_log);
		/* for larger sector sizes, must have v2 or external log */
		ASSERT(log->l_sectbb_log == 0 ||
			log->l_logBBstart == 0 ||
			xfs_sb_version_haslogv2(&mp->m_sb));
		ASSERT(mp->m_sb.sb_logsectlog >= BBSHIFT);
	}
	log->l_sectbb_mask = (1 << log->l_sectbb_log) - 1;

	/*
	 * Find the log head and tail and alert the user to the situation if the
	 * log appears corrupted or contains data. In either case, we do not
	 * proceed past this point unless the user explicitly requests to zap
	 * the log.
	 */
	error = xlog_find_tail(log, &head_blk, &tail_blk);
	if (error) {
		do_warn(
		_("zero_log: cannot find log head/tail (xlog_find_tail=%d)\n"),
			error);
		if (!no_modify && !zap_log) {
			do_warn(_(
"ERROR: The log head and/or tail cannot be discovered. Attempt to mount the\n"
"filesystem to replay the log or use the -L option to destroy the log and\n"
"attempt a repair.\n"));
			exit(2);
		}
	} else {
		if (verbose) {
			do_log(
	_("zero_log: head block %" PRId64 " tail block %" PRId64 "\n"),
				head_blk, tail_blk);
		}
		if (head_blk != tail_blk) {
			if (!no_modify && zap_log) {
				do_warn(_(
"ALERT: The filesystem has valuable metadata changes in a log which is being\n"
"destroyed because the -L option was used.\n"));
			} else if (no_modify) {
				do_warn(_(
"ALERT: The filesystem has valuable metadata changes in a log which is being\n"
"ignored because the -n option was used.  Expect spurious inconsistencies\n"
"which may be resolved by first mounting the filesystem to replay the log.\n"));
			} else {
				do_warn(_(
"ERROR: The filesystem has valuable metadata changes in a log which needs to\n"
"be replayed.  Mount the filesystem to replay the log, and unmount it before\n"
"re-running xfs_repair.  If you are unable to mount the filesystem, then use\n"
"the -L option to destroy the log and attempt a repair.\n"
"Note that destroying the log may cause corruption -- please attempt a mount\n"
"of the filesystem before doing this.\n"));
				exit(2);
			}
		}
	}

	/*
	 * Only clear the log when explicitly requested. Doing so is unnecessary
	 * unless something is wrong. Further, this resets the current LSN of
	 * the filesystem and creates more work for repair of v5 superblock
	 * filesystems.
	 */
	if (!no_modify && zap_log) {
		libxfs_log_clear(log->l_dev, NULL,
			XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart),
			(xfs_extlen_t)XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks),
			&mp->m_sb.sb_uuid,
			xfs_sb_version_haslogv2(&mp->m_sb) ? 2 : 1,
			mp->m_sb.sb_logsunit, XLOG_FMT, XLOG_INIT_CYCLE, true);

		/* update the log data structure with new state */
		error = xlog_find_tail(log, &head_blk, &tail_blk);
		if (error || head_blk != tail_blk)
			do_error(_("failed to clear log"));
	}

	/* And we are now magically complete! */
	PROG_RPT_INC(prog_rpt_done[0], mp->m_sb.sb_logblocks);

	/*
	 * Finally, seed the max LSN from the current state of the log if this
	 * is a v5 filesystem.
	 */
	if (xfs_sb_version_hascrc(&mp->m_sb))
		libxfs_max_lsn = log->l_last_sync_lsn;
}

static bool
set_inobtcount(
	struct xfs_mount	*mp)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Inode btree count feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (!xfs_sb_version_hasfinobt(&mp->m_sb)) {
		printf(
	_("Inode btree count feature requires free inode btree.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasinobtcounts(&mp->m_sb)) {
		printf(_("Filesystem already has inode btree counts.\n"));
		exit(0);
	}

	printf(_("Adding inode btree counts to filesystem.\n"));
	mp->m_sb.sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_INOBTCNT;
	mp->m_sb.sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_bigtime(
	struct xfs_mount	*mp)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Large timestamp feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasbigtime(&mp->m_sb)) {
		printf(_("Filesystem already supports large timestamps.\n"));
		exit(0);
	}

	printf(_("Adding large timestamp support to filesystem.\n"));
	mp->m_sb.sb_features_incompat |= (XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR |
					  XFS_SB_FEAT_INCOMPAT_BIGTIME);
	return true;
}

/* Perform the user's requested upgrades on filesystem. */
static void
upgrade_filesystem(
	struct xfs_mount	*mp)
{
	struct xfs_buf		*bp;
	bool			dirty = false;
	int			error;

	if (add_inobtcount)
		dirty |= set_inobtcount(mp);
	if (add_bigtime)
		dirty |= set_bigtime(mp);

        if (no_modify || !dirty)
                return;

        bp = libxfs_getsb(mp);
        if (!bp || bp->b_error) {
                do_error(
	_("couldn't get superblock for feature upgrade, err=%d\n"),
                                bp ? bp->b_error : ENOMEM);
        } else {
                libxfs_sb_to_disk(bp->b_addr, &mp->m_sb);

                /*
		 * Write the primary super to disk immediately so that
		 * needsrepair will be set if repair doesn't complete.
		 */
                error = -libxfs_bwrite(bp);
                if (error)
                        do_error(
	_("filesystem feature upgrade failed, err=%d\n"),
                                        error);
        }
        if (bp)
                libxfs_buf_relse(bp);
}

/*
 * ok, at this point, the fs is mounted but the root inode may be
 * trashed and the ag headers haven't been checked.  So we have
 * a valid xfs_mount_t and superblock but that's about it.  That
 * means we can use macros that use mount/sb fields in calculations
 * but I/O or btree routines that depend on space maps or inode maps
 * being correct are verboten.
 */

void
phase2(
	struct xfs_mount	*mp,
	int			scan_threads)
{
	int			j;
	ino_tree_node_t		*ino_rec;

	/* now we can start using the buffer cache routines */
	set_mp(mp);

	/* Check whether this fs has internal or external log */
	if (mp->m_sb.sb_logstart == 0) {
		if (!x.logname)
			do_error(_("This filesystem has an external log.  "
				   "Specify log device with the -l option.\n"));

		do_log(_("Phase 2 - using external log on %s\n"), x.logname);
	} else
		do_log(_("Phase 2 - using internal log\n"));

	/* Zero log if applicable */
	do_log(_("        - zero log...\n"));

	set_progress_msg(PROG_FMT_ZERO_LOG, (uint64_t)mp->m_sb.sb_logblocks);
	zero_log(mp);
	print_final_rpt();

	do_log(_("        - scan filesystem freespace and inode maps...\n"));

	bad_ino_btree = 0;

	set_progress_msg(PROG_FMT_SCAN_AG, (uint64_t) glob_agcount);

	scan_ags(mp, scan_threads);

	print_final_rpt();

	/*
	 * make sure we know about the root inode chunk
	 */
	if ((ino_rec = find_inode_rec(mp, 0, mp->m_sb.sb_rootino)) == NULL)  {
		ASSERT(mp->m_sb.sb_rbmino == mp->m_sb.sb_rootino + 1 &&
			mp->m_sb.sb_rsumino == mp->m_sb.sb_rootino + 2);
		do_warn(_("root inode chunk not found\n"));

		/*
		 * mark the first 3 used, the rest are free
		 */
		ino_rec = set_inode_used_alloc(mp, 0,
				(xfs_agino_t) mp->m_sb.sb_rootino);
		set_inode_used(ino_rec, 1);
		set_inode_used(ino_rec, 2);

		for (j = 3; j < XFS_INODES_PER_CHUNK; j++)
			set_inode_free(ino_rec, j);

		/*
		 * also mark blocks
		 */
		set_bmap_ext(0, XFS_INO_TO_AGBNO(mp, mp->m_sb.sb_rootino),
			     M_IGEO(mp)->ialloc_blks, XR_E_INO);
	} else  {
		do_log(_("        - found root inode chunk\n"));

		/*
		 * blocks are marked, just make sure they're in use
		 */
		if (is_inode_free(ino_rec, 0))  {
			do_warn(_("root inode marked free, "));
			set_inode_used(ino_rec, 0);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}

		if (is_inode_free(ino_rec, 1))  {
			do_warn(_("realtime bitmap inode marked free, "));
			set_inode_used(ino_rec, 1);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}

		if (is_inode_free(ino_rec, 2))  {
			do_warn(_("realtime summary inode marked free, "));
			set_inode_used(ino_rec, 2);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}
	}

	/*
	 * Upgrade the filesystem now that we've done a preliminary check of
	 * the superblocks, the AGs, the log, and the metadata inodes.
	 */
	upgrade_filesystem(mp);
}
