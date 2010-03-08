/*
 * linux/fs/next3/snapshot.c
 *
 * Written by Amir Goldstein <amir@ctera.com>, 2008
 *
 * Copyright (C) 2008-2010 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Next3 snapshots core functions.
 */

#include <linux/quotaops.h>
#include "snapshot.h"

/*
 * helper functions
 */

/*
 * use @mask to clear exclude bitmap bits from block bitmap
 * when creating COW bitmap and mark snapshot buffer @sbh uptodate
 */
static inline void
__next3_snapshot_copy_bitmap(struct buffer_head *sbh,
		char *dst, const char *src, const char *mask)
{
	const u32 *ps = (const u32 *)src, *pm = (const u32 *)mask;
	u32 *pd = (u32 *)dst;
	int i;

	for (i = 0; i < SNAPSHOT_ADDR_PER_BLOCK; i++)
		*pd++ = *ps++ & ~*pm++;

	set_buffer_uptodate(sbh);
}

/*
 * copy buffer @bh to (locked) snapshot buffer @sbh and mark it uptodate
 */
static inline void
__next3_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh)
{
	char *src;

	/*
	 * in journaled data mode, @bh can be a user page buffer
	 * that has to be kmapped.
	 */
	src = kmap_atomic(bh->b_page, KM_USER0);
	memcpy(sbh->b_data, src, SNAPSHOT_BLOCK_SIZE);
	kunmap_atomic(src, KM_USER0);
	set_buffer_uptodate(sbh);
}

/*
 * next3_snapshot_complete_cow()
 * Unlock a newly COWed snapshot buffer and complete the COW operation.
 * Optionally, sync the buffer to disk or add it to the current transaction
 * as dirty data.
 */
static inline int
next3_snapshot_complete_cow(handle_t *handle,
		struct buffer_head *sbh, struct buffer_head *bh, int sync)
{
	int err = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_READ
	SNAPSHOT_DEBUG_ONCE;

	/* wait for completion of tracked reads before completing COW */
	while (bh && buffer_tracked_readers_count(bh) > 0) {
		snapshot_debug_once(2, "waiting for tracked reads: "
			    "block = [%lld/%lld], "
			    "tracked_readers_count = %d...\n",
			    SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			    SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			    buffer_tracked_readers_count(bh));
		/*
		 * "This is extremely improbable, so msleep(1) is sufficient
		 *  and there is no need for a wait queue." (dm-snap.c)
		 */
		msleep(1);
	}
#endif

	unlock_buffer(sbh);
	if (handle)
		err = next3_journal_dirty_data(handle, sbh);
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	/* COW operetion is complete */
	next3_snapshot_end_pending_cow(sbh);
#endif
	return err;
}

/*
 * next3_snapshot_copy_buffer_cow()
 * helper function for next3_snapshot_test_and_cow()
 * copy COWed buffer to new allocated (locked) snapshot buffer
 * add complete the COW operation
 */
static inline int
next3_snapshot_copy_buffer_cow(handle_t *handle,
				   struct buffer_head *sbh,
				   struct buffer_head *bh)
{
	__next3_snapshot_copy_buffer(sbh, bh);
	return next3_snapshot_complete_cow(handle, sbh, bh, 0);
}

/*
 * next3_snapshot_copy_buffer()
 * helper function for next3_snapshot_take()
 * used for initializing pre-allocated snapshot blocks
 * copy buffer to snapshot buffer and sync to disk
 * 'mask' block bitmap with exclude bitmap before copying to snapshot.
 */
void next3_snapshot_copy_buffer(struct buffer_head *sbh,
		struct buffer_head *bh, const char *mask)
{
	lock_buffer(sbh);
	if (mask)
		__next3_snapshot_copy_bitmap(sbh,
				sbh->b_data, bh->b_data, mask);
	else
		__next3_snapshot_copy_buffer(sbh, bh);
	unlock_buffer(sbh);
	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
/*
 * XXX: Experimental code
 * next3_snapshot_zero_buffer()
 * helper function for next3_snapshot_test_and_cow()
 * reset snapshot data buffer to zero and
 * add to current transaction (as dirty data)
 * 'blk' is the logical snapshot block number
 * 'blocknr' is the physical block number
 */
static int
next3_snapshot_zero_buffer(handle_t *handle, struct inode *inode,
		next3_snapblk_t blk, next3_fsblk_t blocknr)
{
#pragma ezk skipped this fxn
	int err;
	struct buffer_head *sbh;
	sbh = sb_getblk(inode->i_sb, blocknr);
	if (!sbh)
		return -EIO;

	snapshot_debug(3, "zeroing snapshot block [%lld/%lld] = [%lu/%lu]\n",
			SNAPSHOT_BLOCK_GROUP_OFFSET(blk),
			SNAPSHOT_BLOCK_GROUP(blk),
			SNAPSHOT_BLOCK_GROUP_OFFSET(blocknr),
			SNAPSHOT_BLOCK_GROUP(blocknr));

	lock_buffer(sbh);
	memset(sbh->b_data, 0, SNAPSHOT_BLOCK_SIZE);
	set_buffer_uptodate(sbh);
	unlock_buffer(sbh);
	err = next3_journal_dirty_data(handle, sbh);
	mark_buffer_dirty(sbh);
	brelse(sbh);
	return err;
}
#endif

#define snapshot_debug_hl(n, f, a...)	snapshot_debug_l(n, handle ? 	\
						 handle->h_cowing : 0, f, ## a)

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_FILE_PEEP
/*
 * next3_snapshot_get_inode_access() - called from next3_get_blocks_handle()
 * on snapshot file access.
 * return value <0 indicates access not granted
 * return value 0 indicates normal inode access
 * return value 1 indicates snapshot inode read through access
 * in which case 'prev_snapshot' is pointed to the previous snapshot
 * or set to NULL to indicate read through to block device.
 */
int next3_snapshot_get_inode_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t iblock, int count, int cmd,
		struct inode **prev_snapshot)
{
	struct next3_inode_info *ei = NEXT3_I(inode);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST_PEEP
	struct list_head *prev;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BLOCK
#ifdef CONFIG_NEXT3_FS_DEBUG
	next3_fsblk_t block = SNAPSHOT_BLOCK(iblock);
	unsigned long block_group = (iblock < SNAPSHOT_BLOCK_OFFSET ? -1 :
			SNAPSHOT_BLOCK_GROUP(block));
	next3_grpblk_t blk = (iblock < SNAPSHOT_BLOCK_OFFSET ? iblock :
			SNAPSHOT_BLOCK_GROUP_OFFSET(block));
	snapshot_debug_hl(4, "snapshot (%u) get_blocks [%d/%lu] count=%d "
			  "cmd=%d\n", inode->i_generation, blk, block_group,
			  count, cmd);
#endif

	if (SNAPMAP_ISSPECIAL(cmd)) {
		/*
		 * test_and_cow() COWing or moving blocks to active snapshot
		 */
		BUG_ON(!handle || !handle->h_cowing);
		BUG_ON(!(ei->i_flags & NEXT3_SNAPFILE_ACTIVE_FL));
		BUG_ON(iblock < SNAPSHOT_BLOCK_OFFSET);
		return 0;
	} else if (cmd)
		BUG_ON(handle && handle->h_cowing);
#endif

	if (!(ei->i_flags & NEXT3_SNAPFILE_LIST_FL))
		/*
		 * old snapshot that was removed from the list or new snapshot
		 * that was not added to the list yet (being taken).
		 */
		return 0;

	if (cmd) {
		/* snapshot inode write access */
		snapshot_debug(1, "snapshot (%u) is read-only"
				" - write access denied!\n",
				inode->i_generation);
		return -EPERM;
	} else {
		/* snapshot inode read access */
		if (iblock < SNAPSHOT_BLOCK_OFFSET)
			/* snapshot reserved blocks */
			return 0;
		/*
		 * non NULL handle indicates this is test_and_cow()
		 * checking if snapshot block is mapped
		 */
		if (handle)
			return 0;
	}

	/*
	 * Snapshot image read through access: (!cmd && !handle)
	 * indicates this is next3_snapshot_readpage()
	 * calling next3_snapshot_get_block()
	 */
	*prev_snapshot = NULL;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_LIST_PEEP
	/*
	 * Snapshot list add/delete is protected by lock_super() and we
	 * don't take lock_super() here.  However, since only
	 * old/unused/disabled snapshots are deleted, then we cannot be
	 * affected by deletes here.  If we are following the list
	 * during snapshot take, then we have 3 cases:
	 *
	 * 1. we got here before it was added to list - no problem;
	 * 2. we got here after it was added to list and after it was
	 *    activated - no problem;
	 * 3. we got here after it was added to list and before it was
	 *    activated - we skip it.
	 */
	prev = ei->i_orphan.prev;
	if (list_empty(prev)) {
		/* not in snapshots list */
		return -EIO;
	} else if (prev == &NEXT3_SB(inode->i_sb)->s_snapshot_list) {
		/* last snapshot - read through to block device */
		if (!next3_snapshot_is_active(inode))
			return -EIO;
	} else {
		/* non last snapshot - read through to prev snapshot */
		ei = list_entry(prev, struct next3_inode_info, i_orphan);
		if (!(ei->i_flags & NEXT3_SNAPFILE_LIST_FL))
			/* skip over snapshot during take */
			return 1;
		*prev_snapshot = &ei->vfs_inode;
	}
	return 1;
#else
	return next3_snapshot_is_active(inode) ? 1 : 0;
#endif
}
#endif

/*
 * next3_snapshot_map_blocks() - helper function for
 * next3_snapshot_test_and_cow().  Test if blocks are mapped in snapshot file.
 * If @block is not mapped and if @cmd is non zero, try to allocate @maxblocks.
 * Also used by next3_snapshot_create() to pre-allocate snapshot blocks.
 *
 * Return values:
 * > 0 - no. of mapped blocks in snapshot file
 * = 0 - @block is not mapped in snapshot file
 * < 0 - error
 */
int next3_snapshot_map_blocks(handle_t *handle, struct inode *inode,
			      next3_snapblk_t block, unsigned long maxblocks,
			      next3_fsblk_t *mapped, int cmd)
{
	struct buffer_head dummy;
	int err;

	dummy.b_state = 0;
	dummy.b_blocknr = 0;
	err = next3_get_blocks_handle(handle, inode, SNAPSHOT_IBLOCK(block),
				      maxblocks, &dummy, cmd);
	/*
	 * next3_get_blocks_handle() returns number of blocks
	 * mapped. 0 in case of a HOLE.
	 */
	if (mapped && err > 0)
		*mapped = dummy.b_blocknr;

	snapshot_debug_hl(4, "snapshot (%u) map_blocks "
			  "[%lld/%lld] = [%lld/%lld] "
			  "cmd=%d, maxblocks=%lu, mapped=%d\n",
			  inode->i_generation,
			  SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			  SNAPSHOT_BLOCK_GROUP(block),
			  SNAPSHOT_BLOCK_GROUP_OFFSET(dummy.b_blocknr),
			  SNAPSHOT_BLOCK_GROUP(dummy.b_blocknr),
			  cmd, maxblocks, err);
	return err;
}

/*
 * COW bitmap functions
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_READ
/*
 * next3_snapshot_get_read_access - get read through access to block device.
 * Sanity test to verify that the read block is allocated and not excluded.
 * This test has performance penalty and is only called if SNAPTEST_READ
 * is enabled.  An attempt to read through to block device of a non allocated
 * or excluded block may indicate a corrupted filesystem, corrupted snapshot
 * or corrupted exclude bitmap.  However, it may also be a read-ahead, which
 * was not implicitly requested by the user, so be sure to disable read-ahead
 * on block device (blockdev --setra 0 <bdev>) before enabling SNAPTEST_READ.
 *
 * Return values:
 * = 0 - block is allocated and not excluded
 * < 0 - error (or block is not allocated or excluded)
 */
int next3_snapshot_get_read_access(struct super_block *sb,
				   struct buffer_head *bh)
{
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(bh->b_blocknr);
	next3_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr);
	struct buffer_head *bitmap_bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	int err = 0;

	if (PageReadahead(bh->b_page))
		return 0;

	bitmap_bh = read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	if (!next3_test_bit(bit, bitmap_bh->b_data)) {
		snapshot_debug(2, "warning: attempt to read through to "
				"non-allocated block [%d/%lu] - read ahead?\n",
				bit, block_group);
		brelse(bitmap_bh);
		return -EIO;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
	if (exclude_bitmap_bh &&
		next3_test_bit(bit, exclude_bitmap_bh->b_data)) {
		snapshot_debug(2, "warning: attempt to read through to "
				"excluded block [%d/%lu] - read ahead?\n",
				bit, block_group);
		err = -EIO;
	}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(bitmap_bh);
	return err;
}
#endif

/*
 * next3_snapshot_init_cow_bitmap() init a new allocated (locked) COW bitmap
 * buffer on first time block group access after snapshot take.
 * COW bitmap is created by masking the block bitmap with exclude bitmap.
 */
static int
next3_snapshot_init_cow_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *cow_bh)
{
	struct buffer_head *bitmap_bh;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	struct buffer_head *exclude_bitmap_bh = NULL;
#endif
	char *dst, *src, *mask = NULL;
	struct journal_head *jh;

	bitmap_bh = read_block_bitmap(sb, block_group);
	if (!bitmap_bh)
		return -EIO;

	src = bitmap_bh->b_data;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
	if (exclude_bitmap_bh)
		/* mask block bitmap with exclude bitmap */
		mask = exclude_bitmap_bh->b_data;
#endif
	/*
	 * Another COWing task may be changing this block bitmap
	 * (allocating active snapshot blocks) while we are trying
	 * to copy it.  Copying committed_data will keep us
	 * protected from these changes.  At this point we are
	 * guaranteed that the only difference between block bitmap
	 * and commited_data are the new active snapshot blocks,
	 * because before allocating/freeing any other blocks a task
	 * must first get_undo_access() and get here.
	 */
	jbd_lock_bh_journal_head(bitmap_bh);
	jbd_lock_bh_state(bitmap_bh);
	jh = bh2jh(bitmap_bh);
	if (jh && jh->b_committed_data)
		src = jh->b_committed_data;

	/*
	 * in the path coming from next3_snapshot_read_block_bitmap(),
	 * cow_bh is a user page buffer so it has to be kmapped.
	 */
	dst = kmap_atomic(cow_bh->b_page, KM_USER0);
	__next3_snapshot_copy_bitmap(cow_bh, dst, src, mask);
	kunmap_atomic(dst, KM_USER0);

	jbd_unlock_bh_state(bitmap_bh);
	jbd_unlock_bh_journal_head(bitmap_bh);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
	brelse(exclude_bitmap_bh);
#endif
	brelse(bitmap_bh);
	return 0;
}

/*
 * next3_snapshot_read_block_bitmap()
 * helper function for next3_snapshot_get_block()
 * used for fixing the block bitmap user page buffer when
 * reading through to block device.
 */
int next3_snapshot_read_block_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *bitmap_bh)
{
	int err;

	lock_buffer(bitmap_bh);
	err = next3_snapshot_init_cow_bitmap(sb, block_group, bitmap_bh);
	unlock_buffer(bitmap_bh);
	return err;
}

/*
 * next3_snapshot_read_cow_bitmap - read COW bitmap from active snapshot
 * @handle:	JBD handle
 * @snapshot:	active snapshot
 * @block_group: block group
 *
 * Creates the COW bitmap on first access to @block_group after snapshot take.
 * COW bitmap cache is non-persistent, so no need to mark the group desc
 * block dirty.
 *
 * Return COW bitmap buffer on success or NULL in case of failure.
 */
static struct buffer_head *
next3_snapshot_read_cow_bitmap(handle_t *handle, struct inode *snapshot,
			       unsigned int block_group)
{
	struct super_block *sb = snapshot->i_sb;
	struct next3_sb_info *sbi = NEXT3_SB(sb);
	struct next3_group_desc *desc;
	struct buffer_head *cow_bh;
	next3_fsblk_t bitmap_blk;
	next3_fsblk_t cow_bitmap_blk;
	int err = 0;
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_BITMAP
	SNAPSHOT_DEBUG_ONCE;
#endif

	desc = next3_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;

	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	spin_lock(sb_bgl_lock(sbi, block_group));
	cow_bitmap_blk = le32_to_cpu(desc->bg_cow_bitmap);
	spin_unlock(sb_bgl_lock(sbi, block_group));

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_BITMAP
	/* handle concurrect COW bitmap operations */
	while (cow_bitmap_blk == 0 || cow_bitmap_blk == bitmap_blk) {
		spin_lock(sb_bgl_lock(sbi, block_group));
		cow_bitmap_blk = le32_to_cpu(desc->bg_cow_bitmap);
		if (cow_bitmap_blk == 0)
			/* mark pending COW of bitmap block */
			desc->bg_cow_bitmap = bitmap_blk;
		spin_unlock(sb_bgl_lock(sbi, block_group));

		if (cow_bitmap_blk == 0) {
			snapshot_debug(3, "COWing bitmap #%u of snapshot "
				       "(%u)...\n", block_group,
				       snapshot->i_generation);
			/* sleep 1 tunable delay unit */
			snapshot_test_delay(SNAPTEST_BITMAP);
			break;
		}
		if (cow_bitmap_blk == bitmap_blk) {
			/* wait for another task to COW bitmap block */
			snapshot_debug_once(2, "waiting for pending cow "
					    "bitmap #%d...\n", block_group);
			/*
			 * This is an unlikely event that can happen only once
			 * per block_group/snapshot, so msleep(1) is sufficient
			 * and there is no need for a wait queue.
			 */
			msleep(1);
		}
	}
#endif

	if (cow_bitmap_blk)
		return sb_bread(sb, cow_bitmap_blk);

	/*
	 * Try to read cow bitmap block from snapshot file.  If COW bitmap
	 * is not yet allocated, create the new COW bitmap block.
	 */
	cow_bh = next3_bread(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
				SNAPMAP_READ, &err);
	if (cow_bh)
		goto out;

	/* allocate snapshot block for COW bitmap */
	cow_bh = next3_getblk(handle, snapshot, SNAPSHOT_IBLOCK(bitmap_blk),
				SNAPMAP_BITMAP, &err);
	if (!cow_bh || err < 0)
		goto out;
	if (!err) {
		/*
		 * err should be 1 to indicate new allocated (locked) buffer.
		 * if err is 0, it means that someone mapped this block
		 * before us, while we are updating the COW bitmap cache.
		 * the pending COW bitmap code should prevent that.
		 */
		WARN_ON(1);
		err = -EIO;
		goto out;
	}

	err = next3_snapshot_init_cow_bitmap(sb, block_group, cow_bh);
	if (err)
		goto out;
	/*
	 * complete pending COW operation. no need to wait for tracked reads
	 * of block bitmap, because it is copied directly to page buffer by
	 * next3_snapshot_read_block_bitmap()
	 */
	err = next3_snapshot_complete_cow(handle, cow_bh, NULL, 1);
	if (err)
		goto out;

	snapshot_debug(3, "COW bitmap #%u of snapshot (%u) "
			"mapped to block [%lld/%lld]\n",
			block_group, snapshot->i_generation,
			SNAPSHOT_BLOCK_GROUP_OFFSET(cow_bh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(cow_bh->b_blocknr));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
	handle->h_cow_bitmaps++;
#endif
#endif

out:
	spin_lock(sb_bgl_lock(sbi, block_group));
	if (!err && cow_bh) {
		/* update cow bitmap cache with snapshot cow bitmap block */
		desc->bg_cow_bitmap = cow_bh->b_blocknr;
	} else {
		/* reset cow bitmap cache */
		desc->bg_cow_bitmap = 0;
		brelse(cow_bh);
		cow_bh = NULL;
	}
	spin_unlock(sb_bgl_lock(sbi, block_group));

	if (!cow_bh)
		snapshot_debug(1, "failed to read COW bitmap %u of snapshot "
				"(%u)\n", block_group, snapshot->i_generation);
	return cow_bh;
}

/*
 * next3_snapshot_test_cow_bitmap - test if blocks are in use by snapshot
 * @handle:	JBD handle
 * @snapshot:	active snapshot
 * @block:	address of block
 * @count:	no. of blocks to be tested
 *
 * If the block bit is set in the COW bitmap, than it was allocated at the time
 * that the active snapshot was taken and is therefor "in use" by the snapshot.
 *
 * Return values:
 * > 0 - no. of blocks that are in use by snapshot
 * = 0 - @block is not in use by snapshot
 * < 0 - error
 */
static int
next3_snapshot_test_cow_bitmap(handle_t *handle, struct inode *snapshot,
			       next3_fsblk_t block, int count)
{
	struct buffer_head *cow_bh;
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	next3_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);
	int snapshot_blocks = SNAPSHOT_BLOCKS(snapshot);
	int inuse = 0;

	if (block >= snapshot_blocks)
		/*
		 * Block is not is use by snapshot because it is past the
		 * last f/s block at the time that the snapshot was taken.
		 * (suggests that f/s was resized after snapshot take)
		 */
		return 0;

	cow_bh = next3_snapshot_read_cow_bitmap(handle, snapshot, block_group);
	if (!cow_bh)
		return -EIO;
	/*
	 * if the bit is set in the COW bitmap,
	 * then the block is in use by snapshot
	 */
	while (count > 0 && bit < SNAPSHOT_BLOCKS_PER_GROUP) {
		if (next3_test_bit(bit, cow_bh->b_data))
			inuse++;
		else
			break;
		bit++;
		count--;
	}
	return inuse;
}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
/*
 * next3_snapshot_exclude_blocks() marks blocks in exclude bitmap
 *
 * Return values:
 * >= 0 - no. of blocks set in exclude bitmap
 * < 0 - error
 */
static int
next3_snapshot_exclude_blocks(handle_t *handle, struct super_block *sb,
			      next3_fsblk_t block, int count)
{
	struct buffer_head *exclude_bitmap_bh = NULL;
	unsigned long block_group = SNAPSHOT_BLOCK_GROUP(block);
	next3_grpblk_t bit = SNAPSHOT_BLOCK_GROUP_OFFSET(block);
	int err = 0, n = 0, excluded = 0;

	exclude_bitmap_bh = read_exclude_bitmap(sb, block_group);
	if (!exclude_bitmap_bh)
		return 0;

	err = next3_journal_get_write_access(handle, exclude_bitmap_bh);
	if (err)
		return err;

	while (count > 0 && bit < SNAPSHOT_BLOCKS_PER_GROUP) {
		if (!next3_set_bit_atomic(sb_bgl_lock(NEXT3_SB(sb),
						block_group),
					bit, exclude_bitmap_bh->b_data)) {
			n++;
		} else if (n) {
			snapshot_debug(2, "excluded blocks: [%d-%d/%ld]\n",
					bit-n, bit-1, block_group);
			excluded += n;
			n = 0;
		}
		bit++;
		count--;
	}

	if (n) {
		snapshot_debug(2, "excluded blocks: [%d-%d/%ld]\n",
				bit-n, bit-1, block_group);
		excluded += n;
	}

	if (excluded)
		err = next3_journal_dirty_metadata(handle, exclude_bitmap_bh);

	brelse(exclude_bitmap_bh);
	return err ? err : excluded;
}
#endif

/*
 * COW functions
 */

#ifdef CONFIG_NEXT3_FS_DEBUG
static void
__next3_snapshot_trace_cow(const char *where, handle_t *handle,
		struct super_block *sb, struct inode *inode,
		struct buffer_head *bh, next3_fsblk_t block, int code)
{
	unsigned long inode_group = 0;
	next3_grpblk_t inode_offset = 0;

	if (inode) {
		inode_group = (inode->i_ino - 1) /
			NEXT3_INODES_PER_GROUP(sb);
		inode_offset = (inode->i_ino - 1) %
			NEXT3_INODES_PER_GROUP(sb);
	}
	snapshot_debug_hl(4, "%s(i:%d/%ld, b:%lu/%lu)"
			" h_ref=%d, code=%d\n",
			where, inode_offset, inode_group,
			SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			SNAPSHOT_BLOCK_GROUP(block),
			handle->h_ref, code);
}

#define next3_snapshot_trace_cow(where, handle, sb, inode, bh, block, code) \
	if (snapshot_enable_debug >= 4)					\
		__next3_snapshot_trace_cow(where, handle, sb, inode,	\
				bh, block, code)
#else
#define next3_snapshot_trace_cow(where, handle, sb, inode, bh, block, code)
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CACHE
/*
 * Journal COW cache functions.
 * a block can only be COWed once per snapshot,
 * so a block can only be COWed once per transaction,
 * so a buffer that was COWed in the current transaction,
 * doesn't need to be COWed.
 *
 * Return values:
 * 1 - block was COWed in current transaction
 * 0 - block wasn't COWed in current transaction
 */
static int
next3_snapshot_test_cowed(handle_t *handle, struct buffer_head *bh)
{
	struct journal_head *jh;

	/* check the COW tid in the journal head */
	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && jh->b_cow_tid != handle->h_transaction->t_tid)
			jh = NULL;
		jbd_unlock_bh_state(bh);
		if (jh)
			/*
			 * Block was already COWed in the running transaction,
			 * so we don't need to COW it again.
			 */
			return 1;
	}
	return 0;
}

static void
next3_snapshot_mark_cowed(handle_t *handle, struct buffer_head *bh)
{
	struct journal_head *jh;

	if (bh && buffer_jbd(bh)) {
		jbd_lock_bh_state(bh);
		jh = bh2jh(bh);
		if (jh && jh->b_cow_tid != handle->h_transaction->t_tid) {
			/*
			 * this is the first time this block was COWed
			 * in the runnning transaction.
			 * update the COW tid in the journal head
			 * to mark that this block doesn't need to be COWed.
			 */
			jh->b_cow_tid = handle->h_transaction->t_tid;
		}
		jbd_unlock_bh_state(bh);
	}
}
#endif

enum snapshot_cmd {
	SNAPSHOT_READ,
	SNAPSHOT_COPY,
	SNAPSHOT_MOVE,
	SNAPSHOT_CLEAR
};

/*
 * next3_snapshot_test_and_cow - tests if the blocks should be COWed/moved
 * @where:	name of caller function
 * @handle:	JBD handle
 * @inode:	owner of blocks (NULL for global metadata blocks)
 * @bh:		buffer head of metadata block (NULL for data/deleted block)
 * @block:	address of block
 * @maxblocks:	max. blocks to test/COW/move
 * @cmd:
 *   SNAPSHOT_READ:	return no. of blocks to be COWed/moved
 *   SNAPSHOT_COPY:	copy block to snapshot if needed
 *   SNAPSHOT_MOVE:	move blocks to snapshot if needed
 *   SNAPSHOT_CLEAR:	mark blocks in exclude bitmap
 *
 * Return values:
 * > 0 - no. of blocks that were (or needs to be) moved to snapshot
 * = 0 - @block was COWed or doesn't need to be COWed/moved
 * < 0 - error (or @block needs to be COWed)
 */
static int
next3_snapshot_test_and_cow(const char *where, handle_t *handle,
		struct inode *inode, struct buffer_head *bh,
		next3_fsblk_t block, int maxblocks, enum snapshot_cmd cmd)
{
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
#warning this long function has three distinct modes, based on the cmd.
#warning it should be split into three helpers which can be called from here
#warning or directly from the caller.
#endif
	struct super_block *sb = handle->h_transaction->t_journal->j_private;
	struct inode *active_snapshot = next3_snapshot_has_active(sb);
	struct buffer_head *sbh = NULL;
	next3_fsblk_t blk = 0;
	int err = 0, count = maxblocks;
	int clear = 0;

	if (!active_snapshot)
		/* no active snapshot - no need to COW */
		return 0;

	next3_snapshot_trace_cow(where, handle, sb, inode, bh, block, cmd);

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_INODE
	if (inode && next3_snapshot_exclude_inode(inode)) {
		snapshot_debug_hl(4, "exclude bitmap update - "
				  "skip block cow!\n");
		return 0;
	}
#endif
	if (handle->h_cowing) {
		/* avoid recursion on active snapshot updates */
		WARN_ON(inode && inode != active_snapshot);
		snapshot_debug_hl(4, "active snapshot update - "
				  "skip block cow!\n");
		return 0;
	} else if (inode == active_snapshot) {
		/* active snapshot may only be modified during COW */
		snapshot_debug_hl(4, "active snapshot access denied!\n");
		return -EPERM;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CACHE
	/* check if the buffer was COWed in the current transaction */
	if (next3_snapshot_test_cowed(handle, bh)) {
		snapshot_debug_hl(4, "buffer found in COW cache - "
				  "skip block cow!\n");
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
		handle->h_cow_ok_jh++;
#endif
#endif
		return 0;
	}
#endif

	snapshot_debug_hl(4, "{\n");
	handle->h_cowing = 1;
	/* BEGIN COWing */

	if (inode)
		clear = next3_snapshot_excluded(inode);
	if (clear < 0) {
		/*
		 * excluded file block access - don't COW and
		 * mark block in exclude bitmap
		 */
		snapshot_debug_hl(4, "file (%lu) excluded from snapshot - "
				"mark block (%lu) in exclude bitmap\n",
				inode->i_ino, block);
		cmd = SNAPSHOT_READ;
	}

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CREDITS
	if (!NEXT3_SNAPSHOT_HAS_TRANS_BLOCKS(handle, 1)) {
		/*
		 * The test above is based on lower limit heuristics of
		 * user_credits/buffer_credits, which is not always accurate,
		 * so it is possible that there is no bug here, just another
		 * false alarm.
		 */
		snapshot_debug_hl(1, "COW warning: insuffiecient buffer/user "
				  "credits (%d/%d) for COW operation?\n",
				  handle->h_buffer_credits,
				  handle->h_user_credits);
	}
#endif

	/* get the COW bitmap and test if blocks are in use by snapshot */
	err = next3_snapshot_test_cow_bitmap(handle, active_snapshot,
			block, count);
	if (err < 0)
		goto out;
	if (err && clear < 0) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
		/*
		 * We should never get here because snapshot file blocks should
		 * be excluded from COW bitmap.  The block will not be COWed
		 * anyway, but this can indicate a messed up exclude bitmap.
		 * mark that exclude bitmap needs to be fixed and call
		 * next3_error() which commits the super block.
		 * TODO: implement fix exclude/COW bitmap in fsck.
		 */
		NEXT3_SET_RO_COMPAT_FEATURE(sb,
				NEXT3_FEATURE_RO_COMPAT_FIX_EXCLUDE);
		next3_error(sb, __func__,
			"snapshot file (ino=%lu) block [%lu/%lu] is not "
			"excluded! - run fsck to fix exclude bitmap.\n",
			inode->i_ino,
			SNAPSHOT_BLOCK_GROUP_OFFSET(block),
			SNAPSHOT_BLOCK_GROUP(block));
#endif
		/* don't COW ignored file blocks */
		err = 0;
	}
	if (!err) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
		handle->h_cow_ok_clear++;
#endif
#endif
		goto cowed;
	}

	count = err;
	/* count blocks are in use by snapshot - check if @block is mapped */
	err = next3_snapshot_map_blocks(handle, active_snapshot, block, 1, &blk,
					SNAPMAP_READ);
	if (err < 0)
		goto out;
	if (err > 0) {
		sbh = sb_find_get_block(sb, blk);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
		handle->h_cow_ok_mapped++;
#endif
#endif
		err = 0;
		goto test_pending_cow;
	}

	/* count blocks need to be COWed/moved */
	err = count;
	if (cmd == SNAPSHOT_READ)
		/* don't COW - we were just checking */
		goto out;

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BLOCK_MOVE
	/* if move or delete were requested, try to move blocks to snapshot */
	if (cmd == SNAPSHOT_MOVE) {
		if (inode == NULL) {
			/*
			 * freeing blocks of newly added block group - don't
			 * move them to snapshot
			 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
			handle->h_cow_ok_clear++;
#endif
#endif
			err = 0;
			goto cowed;
		}
		/* try to move count block from inode to snapshot */
		err = next3_snapshot_map_blocks(handle, active_snapshot, block,
						count, NULL, SNAPMAP_MOVE);
		if (err < 0)
			goto out;
		count = err;
		if (count > 0) {
			/*
			 * User should no longer be charged for these blocks.
			 * Snapshot file owner was charged for these blocks
			 * when they were mapped to snapshot file.
			 */
			vfs_dq_free_block(inode, count);
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
			/* set moved blocks in exclude bitmap */
			clear = -1;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
			handle->h_cow_moved += count;
#endif
#endif
			goto cowed;
		}
	}
#endif

	if (cmd != SNAPSHOT_COPY)
		goto out;

	err = -EIO;
	/* make sure we hold an uptodate source buffer */
	if (!bh || !buffer_mapped(bh) || bh->b_blocknr != block)
		goto out;
	if (!buffer_uptodate(bh)) {
		snapshot_debug(1, "warning: non uptodate buffer (%lld)"
				" needs to be copied to active snapshot!\n",
				bh->b_blocknr);
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			goto out;
	}

	/* try to allocate snapshot block to make a backup copy */
	sbh = next3_getblk(handle, active_snapshot, SNAPSHOT_IBLOCK(block),
			   SNAPMAP_COW, &err);
	if (!sbh || err < 0)
		goto out;

	blk = sbh->b_blocknr;
	if (!err) {
		/*
		 * we didn't allocate this block -
		 * another COWing task must have allocated it
		 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
		handle->h_cow_ok_mapped++;
#endif
#endif
		goto test_pending_cow;
	}

	/*
	 * we allocated this block -
	 * copy block data to snapshot and complete COW operation
	 */
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	snapshot_debug(3, "COWing block [%lld/%lld] of snapshot "
			"(%u)...\n",
			SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			active_snapshot->i_generation);
	/* sleep 1 tunable delay unit */
	snapshot_test_delay(SNAPTEST_COW);
#endif
	err = next3_snapshot_copy_buffer_cow(handle, sbh, bh);
	if (err)
		goto out;
	snapshot_debug(3, "block [%lld/%lld] of snapshot (%u) "
			"mapped to block [%lld/%lld]\n",
			SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(bh->b_blocknr),
			active_snapshot->i_generation,
			SNAPSHOT_BLOCK_GROUP_OFFSET(sbh->b_blocknr),
			SNAPSHOT_BLOCK_GROUP(sbh->b_blocknr));
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
	handle->h_cow_copied++;
#endif
#endif

test_pending_cow:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_RACE_COW
	if (sbh)
		/* wait for pending COW to complete */
		next3_snapshot_test_pending_cow(sbh, block);
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_FILES
	if (clear && blk) {
		/*
		 * XXX: Experimental code
		 * zero out snapshot block data
		 */
		err = next3_snapshot_zero_buffer(handle, active_snapshot,
						      bh->b_blocknr, blk);
		if (err)
			goto out;
	}
#endif

cowed:
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_CACHE
	/* mark the buffer COWed in the current transaction */
	next3_snapshot_mark_cowed(handle, bh);
#endif

	if (clear) {
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_EXCLUDE_BITMAP
		/* mark COWed/moved blocks in exclude bitmap */
		clear = next3_snapshot_exclude_blocks(handle, sb,
						      block, count);
		if (clear < 0)
			err = clear;
#endif
#ifdef CONFIG_NEXT3_FS_SNAPSHOT_JOURNAL_TRACE
#ifdef CONFIG_NEXT3_FS_DEBUG
		if (clear > 0)
			handle->h_cow_cleared += clear;
#endif
#endif
	}

out:
	brelse(sbh);
	/* END COWing */
	handle->h_cowing = 0;
	snapshot_debug_hl(4, "} = %d\n", err);
	snapshot_debug_hl(4, ".\n");
	return err;
}

/*
 * tests if the metadata block should be cowed
 */
#define next3_snapshot_test_cow(handle, bh)			\
	next3_snapshot_test_and_cow(__func__, handle, NULL,	\
			bh, bh->b_blocknr, 1, SNAPSHOT_READ)
/*
 * tests if the metadata block should be cowed
 * and in case it does, tries to copy the block to the snapshot
 */
#define next3_snapshot_cow(handle, inode, bh)			\
	next3_snapshot_test_and_cow(__func__, handle, inode,	\
			bh, bh->b_blocknr, 1, SNAPSHOT_COPY)

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_BLOCK_MOVE
/*
 * tests if the data block should be moved
 */
#define next3_snapshot_test_mow(handle, inode, block, num)	\
	next3_snapshot_test_and_cow(__func__, handle, inode,	\
			NULL, block, num, SNAPSHOT_READ)
/*
 * tests if the data block should be moved
 * and in case it does, tries to move the block to the snapshot
 */
#define next3_snapshot_mow(handle, inode, block, num)		\
	next3_snapshot_test_and_cow(__func__, handle, inode,	\
			NULL, block, num, SNAPSHOT_MOVE)
#else
#define next3_snapshot_test_mow(handle, inode, block, num) (num)
#define next3_snapshot_mow(handle, inode, block, num) (num)
#endif
/*
 * tests if the block should be cowed
 * and in case it does, tries to clear the block from the COW bitmap
 */
#define next3_snapshot_clear(handle, inode, block, num)		\
	next3_snapshot_test_and_cow(__func__, handle, inode,	\
			NULL, block, num, SNAPSHOT_CLEAR)

/*
 * Block access functions
 */

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_JBD
/*
 * get_write_access() is called before writing to a metadata block
 * if @inode is not NULL, then this is an inode's indirect block
 * otherwise, this is a file system global metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error (or block needs to be COWed)
 */
int next3_snapshot_get_write_access(handle_t *handle, struct inode *inode,
				    struct buffer_head *bh)
{
	int err = next3_snapshot_cow(handle, inode, bh);

	if (err)
		snapshot_debug(1, "block [%lld/%lld] COW failed!\n",
			       SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			       SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
	return err;
}

/*
 * called from next3_journal_get_undo_access(),
 * which is called for group bitmap block from:
 * 1. next3_free_blocks_sb_inode() before deleting blocks
 * 2. next3_new_blocks() before allocating blocks
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error (or block needs to be COWed)
 */
int next3_snapshot_get_undo_access(handle_t *handle, struct buffer_head *bh)
{
	int err = next3_snapshot_test_cow(handle, bh);

	if (err) {
		/*
		 * We shouldn't get here if everything works properly
		 * because undo access is only requested for block bitmaps
		 * which should be COW'ed in
		 * next3_snapshot_test_cow_bitmap()
		 */
		snapshot_debug(1, "block bitmap [%lld/%lld] COW failed!\n",
			       SNAPSHOT_BLOCK_GROUP_OFFSET(bh->b_blocknr),
			       SNAPSHOT_BLOCK_GROUP(bh->b_blocknr));
		err = -EIO;
	}
	return err;
}

/*
 * get_create_access() is called after allocating a new metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error (or block needs to be COWed)
 */
int next3_snapshot_get_create_access(handle_t *handle, struct buffer_head *bh)
{
	int err;

	err = next3_snapshot_test_cow(handle, bh);
	if (!err)
		return 0;
	/*
	 * We shouldn't get here if get_delete_access() was called for all
	 * deleted blocks.  However, we could definetly get here if fsck was
	 * run and if it had freed some blocks, naturally, without moving
	 * them to snapshot.
	 */
	snapshot_debug(1, "warning: new allocated block (%lld)"
		       " needs to be COWed?!\n", bh->b_blocknr);
	return err;

}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_MOVE
/*
 * get_move_access() - move block to snapshot
 * @handle:	JBD handle
 * @inode:	owner of @block
 * @block:	address of @block
 * @move:	if false, only test if @block needs to be moved
 *
 * Called from next3_get_blocks_handle() before overwriting a data block,
 * when next3_snapshot_should_move_data(@inode) is true.
 * Specifically, only data blocks of regular files, whose data is not being
 * journaled are moved.  Jounraled data blocks are COWed on get_write_access().
 * Snapshots and excluded files blocks are never moved-on-write.
 * If @move is true, then truncate_mutex is held.
 *
 * Return values:
 * = 1 - @block was moved or may not be overwritten
 * = 0 - @block may be overwritten
 * < 0 - error
 */
int next3_snapshot_get_move_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t block, int move)
{
	if (!move)
		return next3_snapshot_test_mow(handle, inode, block, 1);
	else
		return next3_snapshot_mow(handle, inode, block, 1);
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_HOOKS_DELETE
/*
 * get_delete_access() - move count blocks to snapshot
 * @handle:	JBD handle
 * @inode:	owner of blocks
 * @block:	address of start @block
 * @count:	no. of blocks to move
 *
 * Called from next3_free_blocks_sb_inode() before deleting blocks with
 * truncate_mutex held
 *
 * Return values:
 * > 0 - no. of blocks that were moved to snapshot and may not be deleted
 * = 0 - @block may be deleted
 * < 0 - error
 */
int next3_snapshot_get_delete_access(handle_t *handle, struct inode *inode,
		next3_fsblk_t block, int count)
{
	return next3_snapshot_mow(handle, inode, block, count);
}
#endif

#ifdef CONFIG_NEXT3_FS_SNAPSHOT_CLEANUP
/*
 * get_clear_access() - mark snapshot blocks excluded
 * Called from next3_snapshot_clean(), next3_free_branches_cow() and
 * next3_clear_blocks_cow() under snapshot_mutex.
 *
 * Return values:
 * >= 0 - no. of blocks set in exclude bitmap
 * < 0 - error
 */
int next3_snapshot_get_clear_access(handle_t *handle, struct inode *inode,
				    next3_fsblk_t block, int count)
{
	return next3_snapshot_clear(handle, inode, block, count);
}
#endif
