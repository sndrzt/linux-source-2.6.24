/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * file.c
 *
 * File open, close, extend, truncate
 *
 * Copyright (C) 2002, 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/sched.h>
#include <linux/splice.h>
#include <linux/mount.h>
#include <linux/writeback.h>
#include <linux/falloc.h>

#define MLOG_MASK_PREFIX ML_INODE
#include <cluster/masklog.h>

#include "ocfs2.h"

#include "alloc.h"
#include "aops.h"
#include "dir.h"
#include "dlmglue.h"
#include "extent_map.h"
#include "file.h"
#include "sysfile.h"
#include "inode.h"
#include "ioctl.h"
#include "journal.h"
#include "mmap.h"
#include "suballoc.h"
#include "super.h"

#include "buffer_head_io.h"

static int ocfs2_sync_inode(struct inode *inode)
{
	filemap_fdatawrite(inode->i_mapping);
	return sync_mapping_buffers(inode->i_mapping);
}

static int ocfs2_file_open(struct inode *inode, struct file *file)
{
	int status;
	int mode = file->f_flags;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	mlog_entry("(0x%p, 0x%p, '%.*s')\n", inode, file,
		   file->f_path.dentry->d_name.len, file->f_path.dentry->d_name.name);

	spin_lock(&oi->ip_lock);

	/* Check that the inode hasn't been wiped from disk by another
	 * node. If it hasn't then we're safe as long as we hold the
	 * spin lock until our increment of open count. */
	if (OCFS2_I(inode)->ip_flags & OCFS2_INODE_DELETED) {
		spin_unlock(&oi->ip_lock);

		status = -ENOENT;
		goto leave;
	}

	if (mode & O_DIRECT)
		oi->ip_flags |= OCFS2_INODE_OPEN_DIRECT;

	oi->ip_open_count++;
	spin_unlock(&oi->ip_lock);
	status = 0;
leave:
	mlog_exit(status);
	return status;
}

static int ocfs2_file_release(struct inode *inode, struct file *file)
{
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	mlog_entry("(0x%p, 0x%p, '%.*s')\n", inode, file,
		       file->f_path.dentry->d_name.len,
		       file->f_path.dentry->d_name.name);

	spin_lock(&oi->ip_lock);
	if (!--oi->ip_open_count)
		oi->ip_flags &= ~OCFS2_INODE_OPEN_DIRECT;
	spin_unlock(&oi->ip_lock);

	mlog_exit(0);

	return 0;
}

static int ocfs2_sync_file(struct file *file,
			   struct dentry *dentry,
			   int datasync)
{
	int err = 0;
	journal_t *journal;
	struct inode *inode = dentry->d_inode;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	mlog_entry("(0x%p, 0x%p, %d, '%.*s')\n", file, dentry, datasync,
		   dentry->d_name.len, dentry->d_name.name);

	err = ocfs2_sync_inode(dentry->d_inode);
	if (err)
		goto bail;

	journal = osb->journal->j_journal;
	err = journal_force_commit(journal);

bail:
	mlog_exit(err);

	return (err < 0) ? -EIO : 0;
}

int ocfs2_should_update_atime(struct inode *inode,
			      struct vfsmount *vfsmnt)
{
	struct timespec now;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	if (ocfs2_is_hard_readonly(osb) || ocfs2_is_soft_readonly(osb))
		return 0;

	if ((inode->i_flags & S_NOATIME) ||
	    ((inode->i_sb->s_flags & MS_NODIRATIME) && S_ISDIR(inode->i_mode)))
		return 0;

	/*
	 * We can be called with no vfsmnt structure - NFSD will
	 * sometimes do this.
	 *
	 * Note that our action here is different than touch_atime() -
	 * if we can't tell whether this is a noatime mount, then we
	 * don't know whether to trust the value of s_atime_quantum.
	 */
	if (vfsmnt == NULL)
		return 0;

	if ((vfsmnt->mnt_flags & MNT_NOATIME) ||
	    ((vfsmnt->mnt_flags & MNT_NODIRATIME) && S_ISDIR(inode->i_mode)))
		return 0;

	if (vfsmnt->mnt_flags & MNT_RELATIME) {
		if ((timespec_compare(&inode->i_atime, &inode->i_mtime) <= 0) ||
		    (timespec_compare(&inode->i_atime, &inode->i_ctime) <= 0))
			return 1;

		return 0;
	}

	now = CURRENT_TIME;
	if ((now.tv_sec - inode->i_atime.tv_sec <= osb->s_atime_quantum))
		return 0;
	else
		return 1;
}

int ocfs2_update_inode_atime(struct inode *inode,
			     struct buffer_head *bh)
{
	int ret;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	handle_t *handle;
	struct ocfs2_dinode *di = (struct ocfs2_dinode *) bh->b_data;

	mlog_entry_void();

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (handle == NULL) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_journal_access(handle, inode, bh,
				   OCFS2_JOURNAL_ACCESS_WRITE);
	if (ret) {
		mlog_errno(ret);
		goto out_commit;
	}

	/*
	 * Don't use ocfs2_mark_inode_dirty() here as we don't always
	 * have i_mutex to guard against concurrent changes to other
	 * inode fields.
	 */
	inode->i_atime = CURRENT_TIME;
	di->i_atime = cpu_to_le64(inode->i_atime.tv_sec);
	di->i_atime_nsec = cpu_to_le32(inode->i_atime.tv_nsec);

	ret = ocfs2_journal_dirty(handle, bh);
	if (ret < 0)
		mlog_errno(ret);

out_commit:
	ocfs2_commit_trans(OCFS2_SB(inode->i_sb), handle);
out:
	mlog_exit(ret);
	return ret;
}

static int ocfs2_set_inode_size(handle_t *handle,
				struct inode *inode,
				struct buffer_head *fe_bh,
				u64 new_i_size)
{
	int status;

	mlog_entry_void();
	i_size_write(inode, new_i_size);
	inode->i_blocks = ocfs2_inode_sector_count(inode);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;

	status = ocfs2_mark_inode_dirty(handle, inode, fe_bh);
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

bail:
	mlog_exit(status);
	return status;
}

static int ocfs2_simple_size_update(struct inode *inode,
				    struct buffer_head *di_bh,
				    u64 new_i_size)
{
	int ret;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	handle_t *handle = NULL;

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (handle == NULL) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_set_inode_size(handle, inode, di_bh,
				   new_i_size);
	if (ret < 0)
		mlog_errno(ret);

	ocfs2_commit_trans(osb, handle);
out:
	return ret;
}

static int ocfs2_orphan_for_truncate(struct ocfs2_super *osb,
				     struct inode *inode,
				     struct buffer_head *fe_bh,
				     u64 new_i_size)
{
	int status;
	handle_t *handle;
	struct ocfs2_dinode *di;
	u64 cluster_bytes;

	mlog_entry_void();

	/* TODO: This needs to actually orphan the inode in this
	 * transaction. */

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (IS_ERR(handle)) {
		status = PTR_ERR(handle);
		mlog_errno(status);
		goto out;
	}

	status = ocfs2_journal_access(handle, inode, fe_bh,
				      OCFS2_JOURNAL_ACCESS_WRITE);
	if (status < 0) {
		mlog_errno(status);
		goto out_commit;
	}

	/*
	 * Do this before setting i_size.
	 */
	cluster_bytes = ocfs2_align_bytes_to_clusters(inode->i_sb, new_i_size);
	status = ocfs2_zero_range_for_truncate(inode, handle, new_i_size,
					       cluster_bytes);
	if (status) {
		mlog_errno(status);
		goto out_commit;
	}

	i_size_write(inode, new_i_size);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;

	di = (struct ocfs2_dinode *) fe_bh->b_data;
	di->i_size = cpu_to_le64(new_i_size);
	di->i_ctime = di->i_mtime = cpu_to_le64(inode->i_ctime.tv_sec);
	di->i_ctime_nsec = di->i_mtime_nsec = cpu_to_le32(inode->i_ctime.tv_nsec);

	status = ocfs2_journal_dirty(handle, fe_bh);
	if (status < 0)
		mlog_errno(status);

out_commit:
	ocfs2_commit_trans(osb, handle);
out:

	mlog_exit(status);
	return status;
}

static int ocfs2_truncate_file(struct inode *inode,
			       struct buffer_head *di_bh,
			       u64 new_i_size)
{
	int status = 0;
	struct ocfs2_dinode *fe = NULL;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_truncate_context *tc = NULL;

	mlog_entry("(inode = %llu, new_i_size = %llu\n",
		   (unsigned long long)OCFS2_I(inode)->ip_blkno,
		   (unsigned long long)new_i_size);

	fe = (struct ocfs2_dinode *) di_bh->b_data;
	if (!OCFS2_IS_VALID_DINODE(fe)) {
		OCFS2_RO_ON_INVALID_DINODE(inode->i_sb, fe);
		status = -EIO;
		goto bail;
	}

	mlog_bug_on_msg(le64_to_cpu(fe->i_size) != i_size_read(inode),
			"Inode %llu, inode i_size = %lld != di "
			"i_size = %llu, i_flags = 0x%x\n",
			(unsigned long long)OCFS2_I(inode)->ip_blkno,
			i_size_read(inode),
			(unsigned long long)le64_to_cpu(fe->i_size),
			le32_to_cpu(fe->i_flags));

	if (new_i_size > le64_to_cpu(fe->i_size)) {
		mlog(0, "asked to truncate file with size (%llu) to size (%llu)!\n",
		     (unsigned long long)le64_to_cpu(fe->i_size),
		     (unsigned long long)new_i_size);
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	mlog(0, "inode %llu, i_size = %llu, new_i_size = %llu\n",
	     (unsigned long long)le64_to_cpu(fe->i_blkno),
	     (unsigned long long)le64_to_cpu(fe->i_size),
	     (unsigned long long)new_i_size);

	/* lets handle the simple truncate cases before doing any more
	 * cluster locking. */
	if (new_i_size == le64_to_cpu(fe->i_size))
		goto bail;

	down_write(&OCFS2_I(inode)->ip_alloc_sem);

	/* This forces other nodes to sync and drop their pages. Do
	 * this even if we have a truncate without allocation change -
	 * ocfs2 cluster sizes can be much greater than page size, so
	 * we have to truncate them anyway.  */
	status = ocfs2_data_lock(inode, 1);
	if (status < 0) {
		up_write(&OCFS2_I(inode)->ip_alloc_sem);

		mlog_errno(status);
		goto bail;
	}

	unmap_mapping_range(inode->i_mapping, new_i_size + PAGE_SIZE - 1, 0, 1);
	truncate_inode_pages(inode->i_mapping, new_i_size);

	if (OCFS2_I(inode)->ip_dyn_features & OCFS2_INLINE_DATA_FL) {
		status = ocfs2_truncate_inline(inode, di_bh, new_i_size,
					       i_size_read(inode), 1);
		if (status)
			mlog_errno(status);

		goto bail_unlock_data;
	}

	/* alright, we're going to need to do a full blown alloc size
	 * change. Orphan the inode so that recovery can complete the
	 * truncate if necessary. This does the task of marking
	 * i_size. */
	status = ocfs2_orphan_for_truncate(osb, inode, di_bh, new_i_size);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_data;
	}

	status = ocfs2_prepare_truncate(osb, inode, di_bh, &tc);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_data;
	}

	status = ocfs2_commit_truncate(osb, inode, di_bh, tc);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_data;
	}

	/* TODO: orphan dir cleanup here. */
bail_unlock_data:
	ocfs2_data_unlock(inode, 1);

	up_write(&OCFS2_I(inode)->ip_alloc_sem);

bail:

	mlog_exit(status);
	return status;
}

/*
 * extend allocation only here.
 * we'll update all the disk stuff, and oip->alloc_size
 *
 * expect stuff to be locked, a transaction started and enough data /
 * metadata reservations in the contexts.
 *
 * Will return -EAGAIN, and a reason if a restart is needed.
 * If passed in, *reason will always be set, even in error.
 */
int ocfs2_do_extend_allocation(struct ocfs2_super *osb,
			       struct inode *inode,
			       u32 *logical_offset,
			       u32 clusters_to_add,
			       int mark_unwritten,
			       struct buffer_head *fe_bh,
			       handle_t *handle,
			       struct ocfs2_alloc_context *data_ac,
			       struct ocfs2_alloc_context *meta_ac,
			       enum ocfs2_alloc_restarted *reason_ret)
{
	int status = 0;
	int free_extents;
	struct ocfs2_dinode *fe = (struct ocfs2_dinode *) fe_bh->b_data;
	enum ocfs2_alloc_restarted reason = RESTART_NONE;
	u32 bit_off, num_bits;
	u64 block;
	u8 flags = 0;

	BUG_ON(!clusters_to_add);

	if (mark_unwritten)
		flags = OCFS2_EXT_UNWRITTEN;

	free_extents = ocfs2_num_free_extents(osb, inode, fe);
	if (free_extents < 0) {
		status = free_extents;
		mlog_errno(status);
		goto leave;
	}

	/* there are two cases which could cause us to EAGAIN in the
	 * we-need-more-metadata case:
	 * 1) we haven't reserved *any*
	 * 2) we are so fragmented, we've needed to add metadata too
	 *    many times. */
	if (!free_extents && !meta_ac) {
		mlog(0, "we haven't reserved any metadata!\n");
		status = -EAGAIN;
		reason = RESTART_META;
		goto leave;
	} else if ((!free_extents)
		   && (ocfs2_alloc_context_bits_left(meta_ac)
		       < ocfs2_extend_meta_needed(fe))) {
		mlog(0, "filesystem is really fragmented...\n");
		status = -EAGAIN;
		reason = RESTART_META;
		goto leave;
	}

	status = __ocfs2_claim_clusters(osb, handle, data_ac, 1,
					clusters_to_add, &bit_off, &num_bits);
	if (status < 0) {
		if (status != -ENOSPC)
			mlog_errno(status);
		goto leave;
	}

	BUG_ON(num_bits > clusters_to_add);

	/* reserve our write early -- insert_extent may update the inode */
	status = ocfs2_journal_access(handle, inode, fe_bh,
				      OCFS2_JOURNAL_ACCESS_WRITE);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	block = ocfs2_clusters_to_blocks(osb->sb, bit_off);
	mlog(0, "Allocating %u clusters at block %u for inode %llu\n",
	     num_bits, bit_off, (unsigned long long)OCFS2_I(inode)->ip_blkno);
	status = ocfs2_insert_extent(osb, handle, inode, fe_bh,
				     *logical_offset, block, num_bits,
				     flags, meta_ac);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	status = ocfs2_journal_dirty(handle, fe_bh);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	clusters_to_add -= num_bits;
	*logical_offset += num_bits;

	if (clusters_to_add) {
		mlog(0, "need to alloc once more, clusters = %u, wanted = "
		     "%u\n", fe->i_clusters, clusters_to_add);
		status = -EAGAIN;
		reason = RESTART_TRANS;
	}

leave:
	mlog_exit(status);
	if (reason_ret)
		*reason_ret = reason;
	return status;
}

/*
 * For a given allocation, determine which allocators will need to be
 * accessed, and lock them, reserving the appropriate number of bits.
 *
 * Sparse file systems call this from ocfs2_write_begin_nolock()
 * and ocfs2_allocate_unwritten_extents().
 *
 * File systems which don't support holes call this from
 * ocfs2_extend_allocation().
 */
int ocfs2_lock_allocators(struct inode *inode, struct ocfs2_dinode *di,
			  u32 clusters_to_add, u32 extents_to_split,
			  struct ocfs2_alloc_context **data_ac,
			  struct ocfs2_alloc_context **meta_ac)
{
	int ret = 0, num_free_extents;
	unsigned int max_recs_needed = clusters_to_add + 2 * extents_to_split;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	*meta_ac = NULL;
	if (data_ac)
		*data_ac = NULL;

	BUG_ON(clusters_to_add != 0 && data_ac == NULL);

	mlog(0, "extend inode %llu, i_size = %lld, di->i_clusters = %u, "
	     "clusters_to_add = %u, extents_to_split = %u\n",
	     (unsigned long long)OCFS2_I(inode)->ip_blkno, i_size_read(inode),
	     le32_to_cpu(di->i_clusters), clusters_to_add, extents_to_split);

	num_free_extents = ocfs2_num_free_extents(osb, inode, di);
	if (num_free_extents < 0) {
		ret = num_free_extents;
		mlog_errno(ret);
		goto out;
	}

	/*
	 * Sparse allocation file systems need to be more conservative
	 * with reserving room for expansion - the actual allocation
	 * happens while we've got a journal handle open so re-taking
	 * a cluster lock (because we ran out of room for another
	 * extent) will violate ordering rules.
	 *
	 * Most of the time we'll only be seeing this 1 cluster at a time
	 * anyway.
	 *
	 * Always lock for any unwritten extents - we might want to
	 * add blocks during a split.
	 */
	if (!num_free_extents ||
	    (ocfs2_sparse_alloc(osb) && num_free_extents < max_recs_needed)) {
		ret = ocfs2_reserve_new_metadata(osb, di, meta_ac);
		if (ret < 0) {
			if (ret != -ENOSPC)
				mlog_errno(ret);
			goto out;
		}
	}

	if (clusters_to_add == 0)
		goto out;

	ret = ocfs2_reserve_clusters(osb, clusters_to_add, data_ac);
	if (ret < 0) {
		if (ret != -ENOSPC)
			mlog_errno(ret);
		goto out;
	}

out:
	if (ret) {
		if (*meta_ac) {
			ocfs2_free_alloc_context(*meta_ac);
			*meta_ac = NULL;
		}

		/*
		 * We cannot have an error and a non null *data_ac.
		 */
	}

	return ret;
}

static int __ocfs2_extend_allocation(struct inode *inode, u32 logical_start,
				     u32 clusters_to_add, int mark_unwritten)
{
	int status = 0;
	int restart_func = 0;
	int credits;
	u32 prev_clusters;
	struct buffer_head *bh = NULL;
	struct ocfs2_dinode *fe = NULL;
	handle_t *handle = NULL;
	struct ocfs2_alloc_context *data_ac = NULL;
	struct ocfs2_alloc_context *meta_ac = NULL;
	enum ocfs2_alloc_restarted why;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	mlog_entry("(clusters_to_add = %u)\n", clusters_to_add);

	/*
	 * This function only exists for file systems which don't
	 * support holes.
	 */
	BUG_ON(mark_unwritten && !ocfs2_sparse_alloc(osb));

	status = ocfs2_read_block(osb, OCFS2_I(inode)->ip_blkno, &bh,
				  OCFS2_BH_CACHED, inode);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	fe = (struct ocfs2_dinode *) bh->b_data;
	if (!OCFS2_IS_VALID_DINODE(fe)) {
		OCFS2_RO_ON_INVALID_DINODE(inode->i_sb, fe);
		status = -EIO;
		goto leave;
	}

restart_all:
	BUG_ON(le32_to_cpu(fe->i_clusters) != OCFS2_I(inode)->ip_clusters);

	status = ocfs2_lock_allocators(inode, fe, clusters_to_add, 0, &data_ac,
				       &meta_ac);
	if (status) {
		mlog_errno(status);
		goto leave;
	}

	credits = ocfs2_calc_extend_credits(osb->sb, fe, clusters_to_add);
	handle = ocfs2_start_trans(osb, credits);
	if (IS_ERR(handle)) {
		status = PTR_ERR(handle);
		handle = NULL;
		mlog_errno(status);
		goto leave;
	}

restarted_transaction:
	/* reserve a write to the file entry early on - that we if we
	 * run out of credits in the allocation path, we can still
	 * update i_size. */
	status = ocfs2_journal_access(handle, inode, bh,
				      OCFS2_JOURNAL_ACCESS_WRITE);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	prev_clusters = OCFS2_I(inode)->ip_clusters;

	status = ocfs2_do_extend_allocation(osb,
					    inode,
					    &logical_start,
					    clusters_to_add,
					    mark_unwritten,
					    bh,
					    handle,
					    data_ac,
					    meta_ac,
					    &why);
	if ((status < 0) && (status != -EAGAIN)) {
		if (status != -ENOSPC)
			mlog_errno(status);
		goto leave;
	}

	status = ocfs2_journal_dirty(handle, bh);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	spin_lock(&OCFS2_I(inode)->ip_lock);
	clusters_to_add -= (OCFS2_I(inode)->ip_clusters - prev_clusters);
	spin_unlock(&OCFS2_I(inode)->ip_lock);

	if (why != RESTART_NONE && clusters_to_add) {
		if (why == RESTART_META) {
			mlog(0, "restarting function.\n");
			restart_func = 1;
		} else {
			BUG_ON(why != RESTART_TRANS);

			mlog(0, "restarting transaction.\n");
			/* TODO: This can be more intelligent. */
			credits = ocfs2_calc_extend_credits(osb->sb,
							    fe,
							    clusters_to_add);
			status = ocfs2_extend_trans(handle, credits);
			if (status < 0) {
				/* handle still has to be committed at
				 * this point. */
				status = -ENOMEM;
				mlog_errno(status);
				goto leave;
			}
			goto restarted_transaction;
		}
	}

	mlog(0, "fe: i_clusters = %u, i_size=%llu\n",
	     le32_to_cpu(fe->i_clusters),
	     (unsigned long long)le64_to_cpu(fe->i_size));
	mlog(0, "inode: ip_clusters=%u, i_size=%lld\n",
	     OCFS2_I(inode)->ip_clusters, i_size_read(inode));

leave:
	if (handle) {
		ocfs2_commit_trans(osb, handle);
		handle = NULL;
	}
	if (data_ac) {
		ocfs2_free_alloc_context(data_ac);
		data_ac = NULL;
	}
	if (meta_ac) {
		ocfs2_free_alloc_context(meta_ac);
		meta_ac = NULL;
	}
	if ((!status) && restart_func) {
		restart_func = 0;
		goto restart_all;
	}
	if (bh) {
		brelse(bh);
		bh = NULL;
	}

	mlog_exit(status);
	return status;
}

/* Some parts of this taken from generic_cont_expand, which turned out
 * to be too fragile to do exactly what we need without us having to
 * worry about recursive locking in ->prepare_write() and
 * ->commit_write(). */
static int ocfs2_write_zero_page(struct inode *inode,
				 u64 size)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	unsigned long index;
	unsigned int offset;
	handle_t *handle = NULL;
	int ret;

	offset = (size & (PAGE_CACHE_SIZE-1)); /* Within page */
	/* ugh.  in prepare/commit_write, if from==to==start of block, we 
	** skip the prepare.  make sure we never send an offset for the start
	** of a block
	*/
	if ((offset & (inode->i_sb->s_blocksize - 1)) == 0) {
		offset++;
	}
	index = size >> PAGE_CACHE_SHIFT;

	page = grab_cache_page(mapping, index);
	if (!page) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_prepare_write_nolock(inode, page, offset, offset);
	if (ret < 0) {
		mlog_errno(ret);
		goto out_unlock;
	}

	if (ocfs2_should_order_data(inode)) {
		handle = ocfs2_start_walk_page_trans(inode, page, offset,
						     offset);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			handle = NULL;
			goto out_unlock;
		}
	}

	/* must not update i_size! */
	ret = block_commit_write(page, offset, offset);
	if (ret < 0)
		mlog_errno(ret);
	else
		ret = 0;

	if (handle)
		ocfs2_commit_trans(OCFS2_SB(inode->i_sb), handle);
out_unlock:
	unlock_page(page);
	page_cache_release(page);
out:
	return ret;
}

static int ocfs2_zero_extend(struct inode *inode,
			     u64 zero_to_size)
{
	int ret = 0;
	u64 start_off;
	struct super_block *sb = inode->i_sb;

	start_off = ocfs2_align_bytes_to_blocks(sb, i_size_read(inode));
	while (start_off < zero_to_size) {
		ret = ocfs2_write_zero_page(inode, start_off);
		if (ret < 0) {
			mlog_errno(ret);
			goto out;
		}

		start_off += sb->s_blocksize;

		/*
		 * Very large extends have the potential to lock up
		 * the cpu for extended periods of time.
		 */
		cond_resched();
	}

out:
	return ret;
}

int ocfs2_extend_no_holes(struct inode *inode, u64 new_i_size, u64 zero_to)
{
	int ret;
	u32 clusters_to_add;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	clusters_to_add = ocfs2_clusters_for_bytes(inode->i_sb, new_i_size);
	if (clusters_to_add < oi->ip_clusters)
		clusters_to_add = 0;
	else
		clusters_to_add -= oi->ip_clusters;

	if (clusters_to_add) {
		ret = __ocfs2_extend_allocation(inode, oi->ip_clusters,
						clusters_to_add, 0);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}
	}

	/*
	 * Call this even if we don't add any clusters to the tree. We
	 * still need to zero the area between the old i_size and the
	 * new i_size.
	 */
	ret = ocfs2_zero_extend(inode, zero_to);
	if (ret < 0)
		mlog_errno(ret);

out:
	return ret;
}

static int ocfs2_extend_file(struct inode *inode,
			     struct buffer_head *di_bh,
			     u64 new_i_size)
{
	int ret = 0, data_locked = 0;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	BUG_ON(!di_bh);

	/* setattr sometimes calls us like this. */
	if (new_i_size == 0)
		goto out;

	if (i_size_read(inode) == new_i_size)
  		goto out;
	BUG_ON(new_i_size < i_size_read(inode));

	/*
	 * Fall through for converting inline data, even if the fs
	 * supports sparse files.
	 *
	 * The check for inline data here is legal - nobody can add
	 * the feature since we have i_mutex. We must check it again
	 * after acquiring ip_alloc_sem though, as paths like mmap
	 * might have raced us to converting the inode to extents.
	 */
	if (!(oi->ip_dyn_features & OCFS2_INLINE_DATA_FL)
	    && ocfs2_sparse_alloc(OCFS2_SB(inode->i_sb)))
		goto out_update_size;

	/* 
	 * protect the pages that ocfs2_zero_extend is going to be
	 * pulling into the page cache.. we do this before the
	 * metadata extend so that we don't get into the situation
	 * where we've extended the metadata but can't get the data
	 * lock to zero.
	 */
	ret = ocfs2_data_lock(inode, 1);
	if (ret < 0) {
		mlog_errno(ret);
		goto out;
	}
	data_locked = 1;

	/*
	 * The alloc sem blocks people in read/write from reading our
	 * allocation until we're done changing it. We depend on
	 * i_mutex to block other extend/truncate calls while we're
	 * here.
	 */
	down_write(&oi->ip_alloc_sem);

	if (oi->ip_dyn_features & OCFS2_INLINE_DATA_FL) {
		/*
		 * We can optimize small extends by keeping the inodes
		 * inline data.
		 */
		if (ocfs2_size_fits_inline_data(di_bh, new_i_size)) {
			up_write(&oi->ip_alloc_sem);
			goto out_update_size;
		}

		ret = ocfs2_convert_inline_data_to_extents(inode, di_bh);
		if (ret) {
			up_write(&oi->ip_alloc_sem);

			mlog_errno(ret);
			goto out_unlock;
		}
	}

	if (!ocfs2_sparse_alloc(OCFS2_SB(inode->i_sb)))
		ret = ocfs2_extend_no_holes(inode, new_i_size, new_i_size);

	up_write(&oi->ip_alloc_sem);

	if (ret < 0) {
		mlog_errno(ret);
		goto out_unlock;
	}

out_update_size:
	ret = ocfs2_simple_size_update(inode, di_bh, new_i_size);
	if (ret < 0)
		mlog_errno(ret);

out_unlock:
	if (data_locked)
		ocfs2_data_unlock(inode, 1);

out:
	return ret;
}

int ocfs2_setattr(struct dentry *dentry, struct iattr *attr)
{
	int status = 0, size_change;
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct ocfs2_super *osb = OCFS2_SB(sb);
	struct buffer_head *bh = NULL;
	handle_t *handle = NULL;

	mlog_entry("(0x%p, '%.*s')\n", dentry,
	           dentry->d_name.len, dentry->d_name.name);

	if (attr->ia_valid & ATTR_MODE)
		mlog(0, "mode change: %d\n", attr->ia_mode);
	if (attr->ia_valid & ATTR_UID)
		mlog(0, "uid change: %d\n", attr->ia_uid);
	if (attr->ia_valid & ATTR_GID)
		mlog(0, "gid change: %d\n", attr->ia_gid);
	if (attr->ia_valid & ATTR_SIZE)
		mlog(0, "size change...\n");
	if (attr->ia_valid & (ATTR_ATIME | ATTR_MTIME | ATTR_CTIME))
		mlog(0, "time change...\n");

#define OCFS2_VALID_ATTRS (ATTR_ATIME | ATTR_MTIME | ATTR_CTIME | ATTR_SIZE \
			   | ATTR_GID | ATTR_UID | ATTR_MODE)
	if (!(attr->ia_valid & OCFS2_VALID_ATTRS)) {
		mlog(0, "can't handle attrs: 0x%x\n", attr->ia_valid);
		return 0;
	}

	status = inode_change_ok(inode, attr);
	if (status)
		return status;

	size_change = S_ISREG(inode->i_mode) && attr->ia_valid & ATTR_SIZE;
	if (size_change) {
		status = ocfs2_rw_lock(inode, 1);
		if (status < 0) {
			mlog_errno(status);
			goto bail;
		}
	}

	status = ocfs2_meta_lock(inode, &bh, 1);
	if (status < 0) {
		if (status != -ENOENT)
			mlog_errno(status);
		goto bail_unlock_rw;
	}

	if (size_change && attr->ia_size != i_size_read(inode)) {
		if (attr->ia_size > sb->s_maxbytes) {
			status = -EFBIG;
			goto bail_unlock;
		}

		if (i_size_read(inode) > attr->ia_size)
			status = ocfs2_truncate_file(inode, bh, attr->ia_size);
		else
			status = ocfs2_extend_file(inode, bh, attr->ia_size);
		if (status < 0) {
			if (status != -ENOSPC)
				mlog_errno(status);
			status = -ENOSPC;
			goto bail_unlock;
		}
	}

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (IS_ERR(handle)) {
		status = PTR_ERR(handle);
		mlog_errno(status);
		goto bail_unlock;
	}

	/*
	 * This will intentionally not wind up calling vmtruncate(),
	 * since all the work for a size change has been done above.
	 * Otherwise, we could get into problems with truncate as
	 * ip_alloc_sem is used there to protect against i_size
	 * changes.
	 */
	status = inode_setattr(inode, attr);
	if (status < 0) {
		mlog_errno(status);
		goto bail_commit;
	}

	status = ocfs2_mark_inode_dirty(handle, inode, bh);
	if (status < 0)
		mlog_errno(status);

bail_commit:
	ocfs2_commit_trans(osb, handle);
bail_unlock:
	ocfs2_meta_unlock(inode, 1);
bail_unlock_rw:
	if (size_change)
		ocfs2_rw_unlock(inode, 1);
bail:
	if (bh)
		brelse(bh);

	mlog_exit(status);
	return status;
}

int ocfs2_getattr(struct vfsmount *mnt,
		  struct dentry *dentry,
		  struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct ocfs2_super *osb = sb->s_fs_info;
	int err;

	mlog_entry_void();

	err = ocfs2_inode_revalidate(dentry);
	if (err) {
		if (err != -ENOENT)
			mlog_errno(err);
		goto bail;
	}

	generic_fillattr(inode, stat);

	/* We set the blksize from the cluster size for performance */
	stat->blksize = osb->s_clustersize;

bail:
	mlog_exit(err);

	return err;
}

int ocfs2_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	int ret;

	mlog_entry_void();

	ret = ocfs2_meta_lock(inode, NULL, 0);
	if (ret) {
		if (ret != -ENOENT)
			mlog_errno(ret);
		goto out;
	}

	ret = generic_permission(inode, mask, NULL);

	ocfs2_meta_unlock(inode, 0);
out:
	mlog_exit(ret);
	return ret;
}

static int __ocfs2_write_remove_suid(struct inode *inode,
				     struct buffer_head *bh)
{
	int ret;
	handle_t *handle;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_dinode *di;

	mlog_entry("(Inode %llu, mode 0%o)\n",
		   (unsigned long long)OCFS2_I(inode)->ip_blkno, inode->i_mode);

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (handle == NULL) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_journal_access(handle, inode, bh,
				   OCFS2_JOURNAL_ACCESS_WRITE);
	if (ret < 0) {
		mlog_errno(ret);
		goto out_trans;
	}

	inode->i_mode &= ~S_ISUID;
	if ((inode->i_mode & S_ISGID) && (inode->i_mode & S_IXGRP))
		inode->i_mode &= ~S_ISGID;

	di = (struct ocfs2_dinode *) bh->b_data;
	di->i_mode = cpu_to_le16(inode->i_mode);

	ret = ocfs2_journal_dirty(handle, bh);
	if (ret < 0)
		mlog_errno(ret);

out_trans:
	ocfs2_commit_trans(osb, handle);
out:
	mlog_exit(ret);
	return ret;
}

/*
 * Will look for holes and unwritten extents in the range starting at
 * pos for count bytes (inclusive).
 */
static int ocfs2_check_range_for_holes(struct inode *inode, loff_t pos,
				       size_t count)
{
	int ret = 0;
	unsigned int extent_flags;
	u32 cpos, clusters, extent_len, phys_cpos;
	struct super_block *sb = inode->i_sb;

	cpos = pos >> OCFS2_SB(sb)->s_clustersize_bits;
	clusters = ocfs2_clusters_for_bytes(sb, pos + count) - cpos;

	while (clusters) {
		ret = ocfs2_get_clusters(inode, cpos, &phys_cpos, &extent_len,
					 &extent_flags);
		if (ret < 0) {
			mlog_errno(ret);
			goto out;
		}

		if (phys_cpos == 0 || (extent_flags & OCFS2_EXT_UNWRITTEN)) {
			ret = 1;
			break;
		}

		if (extent_len > clusters)
			extent_len = clusters;

		clusters -= extent_len;
		cpos += extent_len;
	}
out:
	return ret;
}

static int ocfs2_write_remove_suid(struct inode *inode)
{
	int ret;
	struct buffer_head *bh = NULL;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	ret = ocfs2_read_block(OCFS2_SB(inode->i_sb),
			       oi->ip_blkno, &bh, OCFS2_BH_CACHED, inode);
	if (ret < 0) {
		mlog_errno(ret);
		goto out;
	}

	ret =  __ocfs2_write_remove_suid(inode, bh);
out:
	brelse(bh);
	return ret;
}

/*
 * Allocate enough extents to cover the region starting at byte offset
 * start for len bytes. Existing extents are skipped, any extents
 * added are marked as "unwritten".
 */
static int ocfs2_allocate_unwritten_extents(struct inode *inode,
					    u64 start, u64 len)
{
	int ret;
	u32 cpos, phys_cpos, clusters, alloc_size;
	u64 end = start + len;
	struct buffer_head *di_bh = NULL;

	if (OCFS2_I(inode)->ip_dyn_features & OCFS2_INLINE_DATA_FL) {
		ret = ocfs2_read_block(OCFS2_SB(inode->i_sb),
				       OCFS2_I(inode)->ip_blkno, &di_bh,
				       OCFS2_BH_CACHED, inode);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}

		/*
		 * Nothing to do if the requested reservation range
		 * fits within the inode.
		 */
		if (ocfs2_size_fits_inline_data(di_bh, end))
			goto out;

		ret = ocfs2_convert_inline_data_to_extents(inode, di_bh);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}
	}

	/*
	 * We consider both start and len to be inclusive.
	 */
	cpos = start >> OCFS2_SB(inode->i_sb)->s_clustersize_bits;
	clusters = ocfs2_clusters_for_bytes(inode->i_sb, start + len);
	clusters -= cpos;

	while (clusters) {
		ret = ocfs2_get_clusters(inode, cpos, &phys_cpos,
					 &alloc_size, NULL);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}

		/*
		 * Hole or existing extent len can be arbitrary, so
		 * cap it to our own allocation request.
		 */
		if (alloc_size > clusters)
			alloc_size = clusters;

		if (phys_cpos) {
			/*
			 * We already have an allocation at this
			 * region so we can safely skip it.
			 */
			goto next;
		}

		ret = __ocfs2_extend_allocation(inode, cpos, alloc_size, 1);
		if (ret) {
			if (ret != -ENOSPC)
				mlog_errno(ret);
			goto out;
		}

next:
		cpos += alloc_size;
		clusters -= alloc_size;
	}

	ret = 0;
out:

	brelse(di_bh);
	return ret;
}

static int __ocfs2_remove_inode_range(struct inode *inode,
				      struct buffer_head *di_bh,
				      u32 cpos, u32 phys_cpos, u32 len,
				      struct ocfs2_cached_dealloc_ctxt *dealloc)
{
	int ret;
	u64 phys_blkno = ocfs2_clusters_to_blocks(inode->i_sb, phys_cpos);
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct inode *tl_inode = osb->osb_tl_inode;
	handle_t *handle;
	struct ocfs2_alloc_context *meta_ac = NULL;
	struct ocfs2_dinode *di = (struct ocfs2_dinode *)di_bh->b_data;

	ret = ocfs2_lock_allocators(inode, di, 0, 1, NULL, &meta_ac);
	if (ret) {
		mlog_errno(ret);
		return ret;
	}

	mutex_lock(&tl_inode->i_mutex);

	if (ocfs2_truncate_log_needs_flush(osb)) {
		ret = __ocfs2_flush_truncate_log(osb);
		if (ret < 0) {
			mlog_errno(ret);
			goto out;
		}
	}

	handle = ocfs2_start_trans(osb, OCFS2_REMOVE_EXTENT_CREDITS);
	if (handle == NULL) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_journal_access(handle, inode, di_bh,
				   OCFS2_JOURNAL_ACCESS_WRITE);
	if (ret) {
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_remove_extent(inode, di_bh, cpos, len, handle, meta_ac,
				  dealloc);
	if (ret) {
		mlog_errno(ret);
		goto out_commit;
	}

	OCFS2_I(inode)->ip_clusters -= len;
	di->i_clusters = cpu_to_le32(OCFS2_I(inode)->ip_clusters);

	ret = ocfs2_journal_dirty(handle, di_bh);
	if (ret) {
		mlog_errno(ret);
		goto out_commit;
	}

	ret = ocfs2_truncate_log_append(osb, handle, phys_blkno, len);
	if (ret)
		mlog_errno(ret);

out_commit:
	ocfs2_commit_trans(osb, handle);
out:
	mutex_unlock(&tl_inode->i_mutex);

	if (meta_ac)
		ocfs2_free_alloc_context(meta_ac);

	return ret;
}

/*
 * Truncate a byte range, avoiding pages within partial clusters. This
 * preserves those pages for the zeroing code to write to.
 */
static void ocfs2_truncate_cluster_pages(struct inode *inode, u64 byte_start,
					 u64 byte_len)
{
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	loff_t start, end;
	struct address_space *mapping = inode->i_mapping;

	start = (loff_t)ocfs2_align_bytes_to_clusters(inode->i_sb, byte_start);
	end = byte_start + byte_len;
	end = end & ~(osb->s_clustersize - 1);

	if (start < end) {
		unmap_mapping_range(mapping, start, end - start, 0);
		truncate_inode_pages_range(mapping, start, end - 1);
	}
}

static int ocfs2_zero_partial_clusters(struct inode *inode,
				       u64 start, u64 len)
{
	int ret = 0;
	u64 tmpend, end = start + len;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	unsigned int csize = osb->s_clustersize;
	handle_t *handle;

	/*
	 * The "start" and "end" values are NOT necessarily part of
	 * the range whose allocation is being deleted. Rather, this
	 * is what the user passed in with the request. We must zero
	 * partial clusters here. There's no need to worry about
	 * physical allocation - the zeroing code knows to skip holes.
	 */
	mlog(0, "byte start: %llu, end: %llu\n",
	     (unsigned long long)start, (unsigned long long)end);

	/*
	 * If both edges are on a cluster boundary then there's no
	 * zeroing required as the region is part of the allocation to
	 * be truncated.
	 */
	if ((start & (csize - 1)) == 0 && (end & (csize - 1)) == 0)
		goto out;

	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (handle == NULL) {
		ret = -ENOMEM;
		mlog_errno(ret);
		goto out;
	}

	/*
	 * We want to get the byte offset of the end of the 1st cluster.
	 */
	tmpend = (u64)osb->s_clustersize + (start & ~(osb->s_clustersize - 1));
	if (tmpend > end)
		tmpend = end;

	mlog(0, "1st range: start: %llu, tmpend: %llu\n",
	     (unsigned long long)start, (unsigned long long)tmpend);

	ret = ocfs2_zero_range_for_truncate(inode, handle, start, tmpend);
	if (ret)
		mlog_errno(ret);

	if (tmpend < end) {
		/*
		 * This may make start and end equal, but the zeroing
		 * code will skip any work in that case so there's no
		 * need to catch it up here.
		 */
		start = end & ~(osb->s_clustersize - 1);

		mlog(0, "2nd range: start: %llu, end: %llu\n",
		     (unsigned long long)start, (unsigned long long)end);

		ret = ocfs2_zero_range_for_truncate(inode, handle, start, end);
		if (ret)
			mlog_errno(ret);
	}

	ocfs2_commit_trans(osb, handle);
out:
	return ret;
}

static int ocfs2_remove_inode_range(struct inode *inode,
				    struct buffer_head *di_bh, u64 byte_start,
				    u64 byte_len)
{
	int ret = 0;
	u32 trunc_start, trunc_len, cpos, phys_cpos, alloc_size;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_cached_dealloc_ctxt dealloc;
	struct address_space *mapping = inode->i_mapping;

	ocfs2_init_dealloc_ctxt(&dealloc);

	if (byte_len == 0)
		return 0;

	if (OCFS2_I(inode)->ip_dyn_features & OCFS2_INLINE_DATA_FL) {
		ret = ocfs2_truncate_inline(inode, di_bh, byte_start,
					    byte_start + byte_len, 0);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}
		/*
		 * There's no need to get fancy with the page cache
		 * truncate of an inline-data inode. We're talking
		 * about less than a page here, which will be cached
		 * in the dinode buffer anyway.
		 */
		unmap_mapping_range(mapping, 0, 0, 0);
		truncate_inode_pages(mapping, 0);
		goto out;
	}

	trunc_start = ocfs2_clusters_for_bytes(osb->sb, byte_start);
	trunc_len = (byte_start + byte_len) >> osb->s_clustersize_bits;
	if (trunc_len >= trunc_start)
		trunc_len -= trunc_start;
	else
		trunc_len = 0;

	mlog(0, "Inode: %llu, start: %llu, len: %llu, cstart: %u, clen: %u\n",
	     (unsigned long long)OCFS2_I(inode)->ip_blkno,
	     (unsigned long long)byte_start,
	     (unsigned long long)byte_len, trunc_start, trunc_len);

	ret = ocfs2_zero_partial_clusters(inode, byte_start, byte_len);
	if (ret) {
		mlog_errno(ret);
		goto out;
	}

	cpos = trunc_start;
	while (trunc_len) {
		ret = ocfs2_get_clusters(inode, cpos, &phys_cpos,
					 &alloc_size, NULL);
		if (ret) {
			mlog_errno(ret);
			goto out;
		}

		if (alloc_size > trunc_len)
			alloc_size = trunc_len;

		/* Only do work for non-holes */
		if (phys_cpos != 0) {
			ret = __ocfs2_remove_inode_range(inode, di_bh, cpos,
							 phys_cpos, alloc_size,
							 &dealloc);
			if (ret) {
				mlog_errno(ret);
				goto out;
			}
		}

		cpos += alloc_size;
		trunc_len -= alloc_size;
	}

	ocfs2_truncate_cluster_pages(inode, byte_start, byte_len);

out:
	ocfs2_schedule_truncate_log_flush(osb, 1);
	ocfs2_run_deallocs(osb, &dealloc);

	return ret;
}

/*
 * Parts of this function taken from xfs_change_file_space()
 */
static int __ocfs2_change_file_space(struct file *file, struct inode *inode,
				     loff_t f_pos, unsigned int cmd,
				     struct ocfs2_space_resv *sr,
				     int change_size)
{
	int ret;
	s64 llen;
	loff_t size;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct buffer_head *di_bh = NULL;
	handle_t *handle;
	unsigned long long max_off = inode->i_sb->s_maxbytes;

	if (ocfs2_is_hard_readonly(osb) || ocfs2_is_soft_readonly(osb))
		return -EROFS;

	mutex_lock(&inode->i_mutex);

	/*
	 * This prevents concurrent writes on other nodes
	 */
	ret = ocfs2_rw_lock(inode, 1);
	if (ret) {
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_meta_lock(inode, &di_bh, 1);
	if (ret) {
		mlog_errno(ret);
		goto out_rw_unlock;
	}

	if (inode->i_flags & (S_IMMUTABLE|S_APPEND)) {
		ret = -EPERM;
		goto out_meta_unlock;
	}

	switch (sr->l_whence) {
	case 0: /*SEEK_SET*/
		break;
	case 1: /*SEEK_CUR*/
		sr->l_start += f_pos;
		break;
	case 2: /*SEEK_END*/
		sr->l_start += i_size_read(inode);
		break;
	default:
		ret = -EINVAL;
		goto out_meta_unlock;
	}
	sr->l_whence = 0;

	llen = sr->l_len > 0 ? sr->l_len - 1 : sr->l_len;

	if (sr->l_start < 0
	    || sr->l_start > max_off
	    || (sr->l_start + llen) < 0
	    || (sr->l_start + llen) > max_off) {
		ret = -EINVAL;
		goto out_meta_unlock;
	}
	size = sr->l_start + sr->l_len;

	if (cmd == OCFS2_IOC_RESVSP || cmd == OCFS2_IOC_RESVSP64) {
		if (sr->l_len <= 0) {
			ret = -EINVAL;
			goto out_meta_unlock;
		}
	}

	if (file && should_remove_suid(file->f_path.dentry)) {
		ret = __ocfs2_write_remove_suid(inode, di_bh);
		if (ret) {
			mlog_errno(ret);
			goto out_meta_unlock;
		}
	}

	down_write(&OCFS2_I(inode)->ip_alloc_sem);
	switch (cmd) {
	case OCFS2_IOC_RESVSP:
	case OCFS2_IOC_RESVSP64:
		/*
		 * This takes unsigned offsets, but the signed ones we
		 * pass have been checked against overflow above.
		 */
		ret = ocfs2_allocate_unwritten_extents(inode, sr->l_start,
						       sr->l_len);
		break;
	case OCFS2_IOC_UNRESVSP:
	case OCFS2_IOC_UNRESVSP64:
		ret = ocfs2_remove_inode_range(inode, di_bh, sr->l_start,
					       sr->l_len);
		break;
	default:
		ret = -EINVAL;
	}
	up_write(&OCFS2_I(inode)->ip_alloc_sem);
	if (ret) {
		mlog_errno(ret);
		goto out_meta_unlock;
	}

	/*
	 * We update c/mtime for these changes
	 */
	handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		mlog_errno(ret);
		goto out_meta_unlock;
	}

	if (change_size && i_size_read(inode) < size)
		i_size_write(inode, size);

	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	ret = ocfs2_mark_inode_dirty(handle, inode, di_bh);
	if (ret < 0)
		mlog_errno(ret);

	ocfs2_commit_trans(osb, handle);

out_meta_unlock:
	brelse(di_bh);
	ocfs2_meta_unlock(inode, 1);
out_rw_unlock:
	ocfs2_rw_unlock(inode, 1);

	mutex_unlock(&inode->i_mutex);
out:
	return ret;
}

int ocfs2_change_file_space(struct file *file, unsigned int cmd,
			    struct ocfs2_space_resv *sr)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);;

	if ((cmd == OCFS2_IOC_RESVSP || cmd == OCFS2_IOC_RESVSP64) &&
	    !ocfs2_writes_unwritten_extents(osb))
		return -ENOTTY;
	else if ((cmd == OCFS2_IOC_UNRESVSP || cmd == OCFS2_IOC_UNRESVSP64) &&
		 !ocfs2_sparse_alloc(osb))
		return -ENOTTY;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	return __ocfs2_change_file_space(file, inode, file->f_pos, cmd, sr, 0);
}

static long ocfs2_fallocate(struct inode *inode, int mode, loff_t offset,
			    loff_t len)
{
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_space_resv sr;
	int change_size = 1;

	if (!ocfs2_writes_unwritten_extents(osb))
		return -EOPNOTSUPP;

	if (S_ISDIR(inode->i_mode))
		return -ENODEV;

	if (mode & FALLOC_FL_KEEP_SIZE)
		change_size = 0;

	sr.l_whence = 0;
	sr.l_start = (s64)offset;
	sr.l_len = (s64)len;

	return __ocfs2_change_file_space(NULL, inode, offset,
					 OCFS2_IOC_RESVSP64, &sr, change_size);
}

static int ocfs2_prepare_inode_for_write(struct dentry *dentry,
					 loff_t *ppos,
					 size_t count,
					 int appending,
					 int *direct_io)
{
	int ret = 0, meta_level = 0;
	struct inode *inode = dentry->d_inode;
	loff_t saved_pos, end;

	/* 
	 * We start with a read level meta lock and only jump to an ex
	 * if we need to make modifications here.
	 */
	for(;;) {
		ret = ocfs2_meta_lock(inode, NULL, meta_level);
		if (ret < 0) {
			meta_level = -1;
			mlog_errno(ret);
			goto out;
		}

		/* Clear suid / sgid if necessary. We do this here
		 * instead of later in the write path because
		 * remove_suid() calls ->setattr without any hint that
		 * we may have already done our cluster locking. Since
		 * ocfs2_setattr() *must* take cluster locks to
		 * proceeed, this will lead us to recursively lock the
		 * inode. There's also the dinode i_size state which
		 * can be lost via setattr during extending writes (we
		 * set inode->i_size at the end of a write. */
		if (should_remove_suid(dentry)) {
			if (meta_level == 0) {
				ocfs2_meta_unlock(inode, meta_level);
				meta_level = 1;
				continue;
			}

			ret = ocfs2_write_remove_suid(inode);
			if (ret < 0) {
				mlog_errno(ret);
				goto out_unlock;
			}
		}

		/* work on a copy of ppos until we're sure that we won't have
		 * to recalculate it due to relocking. */
		if (appending) {
			saved_pos = i_size_read(inode);
			mlog(0, "O_APPEND: inode->i_size=%llu\n", saved_pos);
		} else {
			saved_pos = *ppos;
		}

		end = saved_pos + count;

		/*
		 * Skip the O_DIRECT checks if we don't need
		 * them.
		 */
		if (!direct_io || !(*direct_io))
			break;

		/*
		 * There's no sane way to do direct writes to an inode
		 * with inline data.
		 */
		if (OCFS2_I(inode)->ip_dyn_features & OCFS2_INLINE_DATA_FL) {
			*direct_io = 0;
			break;
		}

		/*
		 * Allowing concurrent direct writes means
		 * i_size changes wouldn't be synchronized, so
		 * one node could wind up truncating another
		 * nodes writes.
		 */
		if (end > i_size_read(inode)) {
			*direct_io = 0;
			break;
		}

		/*
		 * We don't fill holes during direct io, so
		 * check for them here. If any are found, the
		 * caller will have to retake some cluster
		 * locks and initiate the io as buffered.
		 */
		ret = ocfs2_check_range_for_holes(inode, saved_pos, count);
		if (ret == 1) {
			*direct_io = 0;
			ret = 0;
		} else if (ret < 0)
			mlog_errno(ret);
		break;
	}

	if (appending)
		*ppos = saved_pos;

out_unlock:
	ocfs2_meta_unlock(inode, meta_level);

out:
	return ret;
}

static ssize_t ocfs2_file_aio_write(struct kiocb *iocb,
				    const struct iovec *iov,
				    unsigned long nr_segs,
				    loff_t pos)
{
	int ret, direct_io, appending, rw_level, have_alloc_sem  = 0;
	int can_do_direct;
	ssize_t written = 0;
	size_t ocount;		/* original count */
	size_t count;		/* after file limit checks */
	loff_t old_size, *ppos = &iocb->ki_pos;
	u32 old_clusters;
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	mlog_entry("(0x%p, %u, '%.*s')\n", file,
		   (unsigned int)nr_segs,
		   file->f_path.dentry->d_name.len,
		   file->f_path.dentry->d_name.name);

	if (iocb->ki_left == 0)
		return 0;

	vfs_check_frozen(inode->i_sb, SB_FREEZE_WRITE);

	appending = file->f_flags & O_APPEND ? 1 : 0;
	direct_io = file->f_flags & O_DIRECT ? 1 : 0;

	mutex_lock(&inode->i_mutex);

relock:
	/* to match setattr's i_mutex -> i_alloc_sem -> rw_lock ordering */
	if (direct_io) {
		down_read(&inode->i_alloc_sem);
		have_alloc_sem = 1;
	}

	/* concurrent O_DIRECT writes are allowed */
	rw_level = !direct_io;
	ret = ocfs2_rw_lock(inode, rw_level);
	if (ret < 0) {
		mlog_errno(ret);
		goto out_sems;
	}

	can_do_direct = direct_io;
	ret = ocfs2_prepare_inode_for_write(file->f_path.dentry, ppos,
					    iocb->ki_left, appending,
					    &can_do_direct);
	if (ret < 0) {
		mlog_errno(ret);
		goto out;
	}

	/*
	 * We can't complete the direct I/O as requested, fall back to
	 * buffered I/O.
	 */
	if (direct_io && !can_do_direct) {
		ocfs2_rw_unlock(inode, rw_level);
		up_read(&inode->i_alloc_sem);

		have_alloc_sem = 0;
		rw_level = -1;

		direct_io = 0;
		goto relock;
	}

	/*
	 * To later detect whether a journal commit for sync writes is
	 * necessary, we sample i_size, and cluster count here.
	 */
	old_size = i_size_read(inode);
	old_clusters = OCFS2_I(inode)->ip_clusters;

	/* communicate with ocfs2_dio_end_io */
	ocfs2_iocb_set_rw_locked(iocb, rw_level);

	if (direct_io) {
		ret = generic_segment_checks(iov, &nr_segs, &ocount,
					     VERIFY_READ);
		if (ret)
			goto out_dio;

		ret = generic_write_checks(file, ppos, &count,
					   S_ISBLK(inode->i_mode));
		if (ret)
			goto out_dio;

		written = generic_file_direct_write(iocb, iov, &nr_segs, *ppos,
						    ppos, count, ocount);
		if (written < 0) {
			ret = written;
			goto out_dio;
		}
	} else {
		written = generic_file_aio_write_nolock(iocb, iov, nr_segs,
							*ppos);
	}

out_dio:
	/* buffered aio wouldn't have proper lock coverage today */
	BUG_ON(ret == -EIOCBQUEUED && !(file->f_flags & O_DIRECT));

	if ((file->f_flags & O_SYNC && !direct_io) || IS_SYNC(inode)) {
		/*
		 * The generic write paths have handled getting data
		 * to disk, but since we don't make use of the dirty
		 * inode list, a manual journal commit is necessary
		 * here.
		 */
		if (old_size != i_size_read(inode) ||
		    old_clusters != OCFS2_I(inode)->ip_clusters) {
			ret = journal_force_commit(osb->journal->j_journal);
			if (ret < 0)
				written = ret;
		}
	}

	/* 
	 * deep in g_f_a_w_n()->ocfs2_direct_IO we pass in a ocfs2_dio_end_io
	 * function pointer which is called when o_direct io completes so that
	 * it can unlock our rw lock.  (it's the clustered equivalent of
	 * i_alloc_sem; protects truncate from racing with pending ios).
	 * Unfortunately there are error cases which call end_io and others
	 * that don't.  so we don't have to unlock the rw_lock if either an
	 * async dio is going to do it in the future or an end_io after an
	 * error has already done it.
	 */
	if (ret == -EIOCBQUEUED || !ocfs2_iocb_is_rw_locked(iocb)) {
		rw_level = -1;
		have_alloc_sem = 0;
	}

out:
	if (rw_level != -1)
		ocfs2_rw_unlock(inode, rw_level);

out_sems:
	if (have_alloc_sem)
		up_read(&inode->i_alloc_sem);

	mutex_unlock(&inode->i_mutex);

	mlog_exit(ret);
	return written ? written : ret;
}

static ssize_t ocfs2_file_splice_write(struct pipe_inode_info *pipe,
				       struct file *out,
				       loff_t *ppos,
				       size_t len,
				       unsigned int flags)
{
	int ret;
	struct inode *inode = out->f_path.dentry->d_inode;

	mlog_entry("(0x%p, 0x%p, %u, '%.*s')\n", out, pipe,
		   (unsigned int)len,
		   out->f_path.dentry->d_name.len,
		   out->f_path.dentry->d_name.name);

	mutex_lock_nested(&inode->i_mutex, I_MUTEX_PARENT);

	ret = ocfs2_rw_lock(inode, 1);
	if (ret < 0) {
		mlog_errno(ret);
		goto out;
	}

	ret = ocfs2_prepare_inode_for_write(out->f_path.dentry, ppos, len, 0,
					    NULL);
	if (ret < 0) {
		mlog_errno(ret);
		goto out_unlock;
	}

	if (pipe->inode)
		mutex_lock_nested(&pipe->inode->i_mutex, I_MUTEX_CHILD);
	ret = generic_file_splice_write_nolock(pipe, out, ppos, len, flags);
	if (pipe->inode)
		mutex_unlock(&pipe->inode->i_mutex);

out_unlock:
	ocfs2_rw_unlock(inode, 1);
out:
	mutex_unlock(&inode->i_mutex);

	mlog_exit(ret);
	return ret;
}

static ssize_t ocfs2_file_splice_read(struct file *in,
				      loff_t *ppos,
				      struct pipe_inode_info *pipe,
				      size_t len,
				      unsigned int flags)
{
	int ret = 0;
	struct inode *inode = in->f_path.dentry->d_inode;

	mlog_entry("(0x%p, 0x%p, %u, '%.*s')\n", in, pipe,
		   (unsigned int)len,
		   in->f_path.dentry->d_name.len,
		   in->f_path.dentry->d_name.name);

	/*
	 * See the comment in ocfs2_file_aio_read()
	 */
	ret = ocfs2_meta_lock(inode, NULL, 0);
	if (ret < 0) {
		mlog_errno(ret);
		goto bail;
	}
	ocfs2_meta_unlock(inode, 0);

	ret = generic_file_splice_read(in, ppos, pipe, len, flags);

bail:
	mlog_exit(ret);
	return ret;
}

static ssize_t ocfs2_file_aio_read(struct kiocb *iocb,
				   const struct iovec *iov,
				   unsigned long nr_segs,
				   loff_t pos)
{
	int ret = 0, rw_level = -1, have_alloc_sem = 0, lock_level = 0;
	struct file *filp = iocb->ki_filp;
	struct inode *inode = filp->f_path.dentry->d_inode;

	mlog_entry("(0x%p, %u, '%.*s')\n", filp,
		   (unsigned int)nr_segs,
		   filp->f_path.dentry->d_name.len,
		   filp->f_path.dentry->d_name.name);

	if (!inode) {
		ret = -EINVAL;
		mlog_errno(ret);
		goto bail;
	}

	/* 
	 * buffered reads protect themselves in ->readpage().  O_DIRECT reads
	 * need locks to protect pending reads from racing with truncate.
	 */
	if (filp->f_flags & O_DIRECT) {
		down_read(&inode->i_alloc_sem);
		have_alloc_sem = 1;

		ret = ocfs2_rw_lock(inode, 0);
		if (ret < 0) {
			mlog_errno(ret);
			goto bail;
		}
		rw_level = 0;
		/* communicate with ocfs2_dio_end_io */
		ocfs2_iocb_set_rw_locked(iocb, rw_level);
	}

	/*
	 * We're fine letting folks race truncates and extending
	 * writes with read across the cluster, just like they can
	 * locally. Hence no rw_lock during read.
	 * 
	 * Take and drop the meta data lock to update inode fields
	 * like i_size. This allows the checks down below
	 * generic_file_aio_read() a chance of actually working. 
	 */
	ret = ocfs2_meta_lock_atime(inode, filp->f_vfsmnt, &lock_level);
	if (ret < 0) {
		mlog_errno(ret);
		goto bail;
	}
	ocfs2_meta_unlock(inode, lock_level);

	ret = generic_file_aio_read(iocb, iov, nr_segs, iocb->ki_pos);
	if (ret == -EINVAL)
		mlog(ML_ERROR, "generic_file_aio_read returned -EINVAL\n");

	/* buffered aio wouldn't have proper lock coverage today */
	BUG_ON(ret == -EIOCBQUEUED && !(filp->f_flags & O_DIRECT));

	/* see ocfs2_file_aio_write */
	if (ret == -EIOCBQUEUED || !ocfs2_iocb_is_rw_locked(iocb)) {
		rw_level = -1;
		have_alloc_sem = 0;
	}

bail:
	if (have_alloc_sem)
		up_read(&inode->i_alloc_sem);
	if (rw_level != -1) 
		ocfs2_rw_unlock(inode, rw_level);
	mlog_exit(ret);

	return ret;
}

const struct inode_operations ocfs2_file_iops = {
	.setattr	= ocfs2_setattr,
	.getattr	= ocfs2_getattr,
	.permission	= ocfs2_permission,
	.fallocate	= ocfs2_fallocate,
};

const struct inode_operations ocfs2_special_file_iops = {
	.setattr	= ocfs2_setattr,
	.getattr	= ocfs2_getattr,
	.permission	= ocfs2_permission,
};

const struct file_operations ocfs2_fops = {
	.read		= do_sync_read,
	.write		= do_sync_write,
	.mmap		= ocfs2_mmap,
	.fsync		= ocfs2_sync_file,
	.release	= ocfs2_file_release,
	.open		= ocfs2_file_open,
	.aio_read	= ocfs2_file_aio_read,
	.aio_write	= ocfs2_file_aio_write,
	.ioctl		= ocfs2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = ocfs2_compat_ioctl,
#endif
	.splice_read	= ocfs2_file_splice_read,
	.splice_write	= ocfs2_file_splice_write,
};

const struct file_operations ocfs2_dops = {
	.read		= generic_read_dir,
	.readdir	= ocfs2_readdir,
	.fsync		= ocfs2_sync_file,
	.ioctl		= ocfs2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = ocfs2_compat_ioctl,
#endif
};
