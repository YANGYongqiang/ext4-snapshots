/*
 * linux/fs/ext4/snapshot.h
 *
 * Written by Amir Goldstein <amir73il@users.sf.net>, 2008
 *
 * Copyright (C) 2008-2011 CTERA Networks
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 snapshot extensions.
 */

#ifndef _LINUX_EXT4_SNAPSHOT_H
#define _LINUX_EXT4_SNAPSHOT_H

#include <linux/version.h>
#include <linux/delay.h>
#include "ext4.h"
#include "snapshot_debug.h"


/*
 * We assert that file system block size == page size (on mount time)
 * and that the first file system block is block 0 (on snapshot create).
 * Snapshot inode direct blocks are reserved for snapshot meta blocks.
 * Snapshot inode single indirect blocks are not used.
 * Snapshot image starts at the first double indirect block, so all blocks in
 * Snapshot image block group blocks are mapped by a single DIND block:
 * 4k: 32k blocks_per_group = 32 IND (4k) blocks = 32 groups per DIND
 * 8k: 64k blocks_per_group = 32 IND (8k) blocks = 64 groups per DIND
 * 16k: 128k blocks_per_group = 32 IND (16k) blocks = 128 groups per DIND
 */
#define SNAPSHOT_BLOCK_SIZE		PAGE_SIZE
#define SNAPSHOT_BLOCK_SIZE_BITS	PAGE_SHIFT
#define SNAPSHOT_ADDR_PER_BLOCK_BITS	(SNAPSHOT_BLOCK_SIZE_BITS - 2)
#define	SNAPSHOT_ADDR_PER_BLOCK		(1 << SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_DIR_BLOCKS		EXT4_NDIR_BLOCKS
#define SNAPSHOT_IND_BLOCKS		SNAPSHOT_ADDR_PER_BLOCK
#define SNAPSHOT_BLOCK_OFFSET					\
	(SNAPSHOT_DIR_BLOCKS + SNAPSHOT_IND_BLOCKS)

#define SNAPSHOT_BLOCKS_PER_GROUP_BITS	(SNAPSHOT_BLOCK_SIZE_BITS + 3)
#define SNAPSHOT_BLOCKS_PER_GROUP				\
	(1 << SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP(block)				\
	((block) >> SNAPSHOT_BLOCKS_PER_GROUP_BITS)
#define SNAPSHOT_BLOCK_GROUP_OFFSET(block)			\
	((block) & (SNAPSHOT_BLOCKS_PER_GROUP - 1))
#define SNAPSHOT_BLOCK_TUPLE(block)				\
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP_OFFSET(block),	\
	(ext4_fsblk_t)SNAPSHOT_BLOCK_GROUP(block)
#define SNAPSHOT_IND_PER_BLOCK_GROUP_BITS			\
	(SNAPSHOT_BLOCKS_PER_GROUP_BITS - SNAPSHOT_ADDR_PER_BLOCK_BITS)
#define SNAPSHOT_IND_PER_BLOCK_GROUP				\
	(1 << SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS_BITS				\
	(SNAPSHOT_ADDR_PER_BLOCK_BITS - SNAPSHOT_IND_PER_BLOCK_GROUP_BITS)
#define SNAPSHOT_DIND_BLOCK_GROUPS				\
	(1 << SNAPSHOT_DIND_BLOCK_GROUPS_BITS)

/*
 * A snapshot file maps physical block addresses in the snapshot image
 * to logical block offsets in the snapshot file. Even though the mapping
 * is trivial (iblock = block), the type casting is still needed, because
 * the addresses are in a different address space.
 */
#define SNAPSHOT_BLOCK(iblock)	((ext4_fsblk_t)(iblock))
#define SNAPSHOT_IBLOCK(block)	(ext4_lblk_t)((block))



#ifdef CONFIG_EXT4_FS_SNAPSHOT
#define EXT4_SNAPSHOT_VERSION "ext4 snapshot v1.0.13-7 (30-Oct-2011)"

#define SNAPSHOT_BYTES_OFFSET					\
	(SNAPSHOT_BLOCK_OFFSET << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_ISIZE(size)					\
	((size) + SNAPSHOT_BYTES_OFFSET)
/* Snapshot block device size is recorded in i_disksize */
#define SNAPSHOT_SET_SIZE(inode, size)				\
	(EXT4_I(inode)->i_disksize = SNAPSHOT_ISIZE(size))
#define SNAPSHOT_SIZE(inode)					\
	(EXT4_I(inode)->i_disksize - SNAPSHOT_BYTES_OFFSET)
#define SNAPSHOT_SET_BLOCKS(inode, blocks)			\
	SNAPSHOT_SET_SIZE((inode),				\
			(loff_t)(blocks) << SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_BLOCKS(inode)					\
	(ext4_fsblk_t)(SNAPSHOT_SIZE(inode) >> SNAPSHOT_BLOCK_SIZE_BITS)
/* Snapshot shrink/merge/clean progress is exported via i_size */
#define SNAPSHOT_PROGRESS(inode)				\
	(ext4_fsblk_t)((inode)->i_size >> SNAPSHOT_BLOCK_SIZE_BITS)
#define SNAPSHOT_SET_ENABLED(inode)				\
	i_size_write((inode), SNAPSHOT_SIZE(inode))
#define SNAPSHOT_SET_PROGRESS(inode, blocks)			\
	snapshot_size_extend((inode), (blocks))
/* Disabled/deleted snapshot i_size is 1 block, to allow read of super block */
#define SNAPSHOT_SET_DISABLED(inode)				\
	snapshot_size_truncate((inode), 1)
/* Removed snapshot i_size and i_disksize are 0, since all blocks were freed */
#define SNAPSHOT_SET_REMOVED(inode)				\
	do {							\
		EXT4_I(inode)->i_disksize = 0;			\
		snapshot_size_truncate((inode), 0);		\
	} while (0)

static inline void snapshot_size_extend(struct inode *inode,
			ext4_fsblk_t blocks)
{
#ifdef CONFIG_EXT4_FS_SNAPSHOT_DEBUG
#ifdef CONFIG_EXT4_DEBUG
	ext4_fsblk_t old_blocks = SNAPSHOT_PROGRESS(inode);
	ext4_fsblk_t max_blocks = SNAPSHOT_BLOCKS(inode);

	/* sleep total of tunable delay unit over 100% progress */
	snapshot_test_delay_progress(SNAPTEST_DELETE,
			old_blocks, blocks, max_blocks);
#endif
#endif
	i_size_write((inode), (loff_t)(blocks) << SNAPSHOT_BLOCK_SIZE_BITS);
}

static inline void snapshot_size_truncate(struct inode *inode,
			ext4_fsblk_t blocks)
{
	loff_t i_size = (loff_t)blocks << SNAPSHOT_BLOCK_SIZE_BITS;

	i_size_write(inode, i_size);
	truncate_inode_pages(&inode->i_data, i_size);
}

/* Is ext4 configured for snapshots support? */
static inline int EXT4_SNAPSHOTS(struct super_block *sb)
{
	return EXT4_HAS_RO_COMPAT_FEATURE(sb,
			EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT);
}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK
/*
 * snapshot_map_blocks() command flags passed to ext4_map_blocks() on its
 * @flags argument. The higher bits are used for mapping snapshot blocks.
 */
/* original meaning - only check if blocks are mapped */
#define SNAPMAP_READ	0
/* original meaning - allocate missing blocks and indirect blocks */
#define SNAPMAP_WRITE	EXT4_GET_BLOCKS_CREATE
/* creating COWed block */
#define SNAPMAP_COW	(SNAPMAP_WRITE|EXT4_GET_BLOCKS_COW)
/* moving blocks to snapshot */
#define SNAPMAP_MOVE	(SNAPMAP_WRITE|EXT4_GET_BLOCKS_MOVE)
 /* creating COW bitmap - handle COW races and bypass journal */
#define SNAPMAP_BITMAP	(SNAPMAP_COW|EXT4_GET_BLOCKS_SYNC)

/* test special cases when mapping snapshot blocks */
#define SNAPMAP_ISCOW(cmd)	(unlikely((cmd) & EXT4_GET_BLOCKS_COW))
#define SNAPMAP_ISMOVE(cmd)	(unlikely((cmd) & EXT4_GET_BLOCKS_MOVE))
#define SNAPMAP_ISSYNC(cmd)	(unlikely((cmd) & EXT4_GET_BLOCKS_SYNC))

#define IS_COWING(handle)	(unlikely((handle)->h_cowing))

/* snapshot.c */

/* helper functions for ext4_snapshot_create() */
extern int ext4_snapshot_map_blocks(handle_t *handle, struct inode *inode,
				     ext4_fsblk_t block,
				     unsigned long maxblocks,
				     ext4_fsblk_t *mapped, int cmd);
/* helper function for ext4_snapshot_take() */
extern void ext4_snapshot_copy_buffer(struct buffer_head *sbh,
					   struct buffer_head *bh,
					   const char *mask);
/* helper function for ext4_snapshot_get_block() */
extern int ext4_snapshot_read_block_bitmap(struct super_block *sb,
		unsigned int block_group, struct buffer_head *bitmap_bh);

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_COW
extern int ext4_snapshot_test_and_cow(const char *where,
		handle_t *handle, struct inode *inode,
		ext4_fsblk_t block, struct buffer_head *bh, int cow);

/*
 * test if a metadata block should be COWed
 * and if it should, copy the block to the active snapshot
 */
#define ext4_snapshot_cow(handle, inode, block, bh, cow)	\
	ext4_snapshot_test_and_cow(__func__, handle, inode,	\
			block, bh, cow)
#else
#define ext4_snapshot_cow(handle, inode, block, bh, cow) 0
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_BLOCK_MOVE
extern int ext4_snapshot_test_and_move(const char *where,
		handle_t *handle, struct inode *inode,
		ext4_fsblk_t block, int *pcount, int move);

/*
 * test if blocks should be moved to snapshot
 * and if they should, try to move them to the active snapshot
 */
#define ext4_snapshot_move(handle, inode, block, pcount, move)	\
	ext4_snapshot_test_and_move(__func__, handle, inode,	\
			block, pcount, move)
#else
#define ext4_snapshot_move(handle, inode, block, pcount, move) (0)
#endif

extern void ext4_snapshot_fix_group_counters(struct super_block *sb,
					struct buffer_head *block_bitmap,
					struct ext4_group_desc *gdp,
					ext4_group_t group);
/*
 * Block access functions
 */

#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_JBD
/*
 * get_write_access() is called before writing to a metadata block
 * if @inode is not NULL, then this is an inode's indirect block
 * otherwise, this is a file system global metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_write_access(handle_t *handle,
		struct inode *inode, struct buffer_head *bh)
{
	struct super_block *sb;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	return ext4_snapshot_cow(handle, inode, bh->b_blocknr, bh, 1);
}

/*
 * get_create_access() is called after allocating a new metadata block
 *
 * Return values:
 * = 0 - block was COWed or doesn't need to be COWed
 * < 0 - error
 */
static inline int ext4_snapshot_get_create_access(handle_t *handle,
		struct buffer_head *bh)
{
	struct super_block *sb;
	int err;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	/* Should block be COWed? */
	err = ext4_snapshot_cow(handle, NULL, bh->b_blocknr, bh, 0);
	/*
	 * A new block shouldn't need to be COWed if get_delete_access() was
	 * called for all deleted blocks.  However, it may need to be COWed
	 * if fsck was run and if it had freed some blocks without moving them
	 * to snapshot.  In the latter case, -EIO will be returned.
	 */
	if (err > 0)
		err = -EIO;
	return err;
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_BITMAP
/*
 * get_bitmap_access() is called before modifying a block bitmap.
 * this call initializes the COW bitmap for @group.
 *
 * Return values:
 * = 0 - COW bitmap is initialized
 * < 0 - error
 */
static inline int ext4_snapshot_get_bitmap_access(handle_t *handle,
		struct super_block *sb, ext4_group_t group,
		struct buffer_head *bh)
{
	if (!EXT4_SNAPSHOTS(sb))
		return 0;
	/*
	 * With flex_bg, block bitmap may reside in a different group than
	 * the group it describes, so we need to init both COW bitmaps:
	 * 1. init the COW bitmap for @group by testing
	 *    if the first block in the group should be COWed
	 */
	if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG)) {
		int err = ext4_snapshot_cow(handle, NULL,
				ext4_group_first_block_no(sb, group),
				NULL, 0);
		if (err < 0)
			return err;
	}
	/* 2. COW the block bitmap itself, which may be in another group */
	return ext4_snapshot_cow(handle, NULL, bh->b_blocknr, bh, 1);
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DATA
/*
 * get_move_access() - move block to snapshot
 * @handle:	JBD handle
 * @inode:	owner of @block
 * @block:	address of @block
 * @pcount      pointer to no. of blocks about to move or approve
 * @move:	if false, only test if blocks need to be moved
 *
 * Called from ext4_ind_map_blocks() before overwriting a data block, when the
 * buffer_move_on_write() flag is set.  Specifically, only data blocks of
 * regular files are moved. Directory blocks are COWed on get_write_access().
 * Snapshots and excluded files blocks are never moved-on-write.
 * If @move is true, then down_write(&i_data_sem) is held.
 *
 * Return values:
 * > 0 - @blocks a) were moved for @move = 1;
 *		 b) need to be moved for @move = 0.
 * = 0 - blocks don't need to be moved.
 * < 0 - error
 */
static inline int ext4_snapshot_get_move_access(handle_t *handle,
						struct inode *inode,
						ext4_fsblk_t block,
						int *pcount, int move)
{
	if (!EXT4_SNAPSHOTS(inode->i_sb))
		return 0;

	return ext4_snapshot_move(handle, inode, block, pcount, move);
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_HOOKS_DELETE
/*
 * get_delete_access() - move blocks to snapshot or approve to free them
 * @handle:	JBD handle
 * @inode:	owner of blocks if known (or NULL otherwise)
 * @block:	address of start @block
 * @pcount:	pointer to no. of blocks about to move or approve
 *
 * Called from ext4_free_blocks() before deleting blocks with
 * i_data_sem held
 *
 * Return values:
 * > 0 - blocks were moved to snapshot and may not be freed
 * = 0 - blocks may be freed
 * < 0 - error
 */
static inline int ext4_snapshot_get_delete_access(handle_t *handle,
		struct inode *inode, ext4_fsblk_t block, int *pcount)
{
	struct super_block *sb;

	sb = handle->h_transaction->t_journal->j_private;
	if (!EXT4_SNAPSHOTS(sb))
		return 0;

	return ext4_snapshot_move(handle, inode, block, pcount, 1);
}
#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_EXCLUDE_BITMAP
extern int ext4_snapshot_test_and_exclude(const char *where, handle_t *handle,
		struct super_block *sb, ext4_fsblk_t block, int maxblocks,
		int exclude);

/*
 * ext4_snapshot_exclude_blocks() - exclude snapshot blocks
 *
 * Called from ext4_snapshot_test_and_{cow,move}() when copying/moving
 * blocks to active snapshot.
 *
 * Return <0 on error.
 */
#define ext4_snapshot_exclude_blocks(handle, sb, block, count) \
	ext4_snapshot_test_and_exclude(__func__, (handle), (sb), \
			(block), (count), 1)

/*
 * ext4_snapshot_test_excluded() - test that snapshot blocks are excluded
 *
 * Called from ext4_count_branches() and
 * ext4_count_blocks() under snapshot_mutex.
 *
 * Return <0 on error or if snapshot blocks are not excluded.
 */
#define ext4_snapshot_test_excluded(inode, block, count) \
	ext4_snapshot_test_and_exclude(__func__, NULL, (inode)->i_sb, \
			(block), (count), 0)

#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
extern void init_ext4_snapshot_cow_cache(void);
#endif

/* snapshot_ctl.c */
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CTL

/*
 * Snapshot control functions
 */
extern void ext4_snapshot_get_flags(struct inode *inode, struct file *filp);
extern int ext4_snapshot_set_flags(handle_t *handle, struct inode *inode,
				    unsigned int flags);
extern int ext4_snapshot_take(struct inode *inode);
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * Snapshot constructor/destructor
 */
extern int ext4_snapshot_load(struct super_block *sb,
		struct ext4_super_block *es, int read_only);
extern int ext4_snapshot_update(struct super_block *sb, int cleanup,
		int read_only);
extern void ext4_snapshot_destroy(struct super_block *sb);
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
extern int ext4_snapclone_take(struct dentry *old_dentry, struct inode *dir,
			struct dentry *dentry);
extern long ext4_snapclone_load(struct inode *);
extern void ext4_snapclone_destroy(struct inode *);
extern int ext4_snapclone_set_flags(handle_t *handle, struct inode *inode,
				    unsigned int flags);
#endif

static inline int init_ext4_snapshot(void)
{
#ifdef CONFIG_EXT4_FS_SNAPSHOT_JOURNAL_CACHE
	init_ext4_snapshot_cow_cache();
#endif
	return 0;
}

static inline void exit_ext4_snapshot(void)
{
}

#ifdef CONFIG_EXT4_FS_SNAPSHOT_CLEANUP_SHRINK
/* snapshot_inode.c */
extern int ext4_snapshot_shrink_blocks(handle_t *handle, struct inode *inode,
		ext4_lblk_t iblock, unsigned long maxblocks,
		struct buffer_head *cow_bh,
		int shrink, int *pmapped);
#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_CLEANUP_MERGE
extern int ext4_snapshot_merge_blocks(handle_t *handle,
		struct inode *src, struct inode *dst,
		ext4_lblk_t iblock, unsigned long maxblocks);
#endif

#ifdef CONFIG_EXT4_FS_SNAPCLONE_FILE
extern int ext4_snapshot_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create);
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/* tests if @inode is a snapshot file */
static inline int ext4_snapshot_file(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		/* a snapshots directory */
		return 0;
	return ext4_test_inode_flag(inode, EXT4_INODE_SNAPFILE);
}

/* tests if @inode is on the on-disk snapshot list */
static inline int ext4_snapshot_list(struct inode *inode)
{
	return ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_LIST);
}
#endif
#ifdef CONFIG_EXT4_FS_SNAPCLONE_FILE
/* tests if @inode is cloned */
static inline int ext4_snapshot_cloned(struct inode *inode)
{
	return ext4_test_inode_snapstate(inode, EXT4_SNAPSTATE_CLONED);
}

/* tests if @inode is a snapclone file */
static inline int ext4_snapclone_file(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		/* a snapclone directory */
		return 0;
	return ext4_test_inode_flag(inode, EXT4_INODE_SNAPCLONE);
}
#else
#define ext4_snapclone_file(inode)	(0)
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/*
 * ext4_snapshot_excluded():
 * Checks if the file should be excluded from snapshot.
 *
 * Returns 0 for normal file.
 * Returns > 0 for 'excluded' file.
 * Returns < 0 for 'ignored' file (stonger than 'excluded').
 *
 * Excluded and ignored file blocks are not moved to snapshot.
 * Ignored file metadata blocks are not COWed to snapshot.
 * Excluded file metadata blocks are zeroed in the snapshot file.
 * XXX: Excluded files code is experimental,
 *      but ignored files code isn't.
 */
static inline int ext4_snapshot_excluded(struct inode *inode)
{
	/* directory blocks and global filesystem blocks cannot be 'excluded' */
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;
	/* snapshot files are 'ignored' */
	if (ext4_snapshot_file(inode))
		return -1;
	return 0;
}
#endif

#ifdef CONFIG_EXT4_FS_SNAPSHOT_FILE
/* tests if the file system has an active snapshot */
static inline int ext4_snapshot_active(struct ext4_sb_info *sbi)
{
	if (unlikely((sbi)->s_active_snapshot))
		return 1;
	return 0;
}

/*
 * tests if the file system has an active snapshot and returns its inode.
 * active snapshot is only changed under journal_lock_updates(),
 * so it is safe to use the returned inode during a transaction.
 */
static inline struct inode *ext4_snapshot_has_active(struct super_block *sb)
{
	return EXT4_SB(sb)->s_active_snapshot;
}

/*
 * tests if @inode is the current active snapshot.
 * active snapshot is only changed under journal_lock_updates(),
 * so the test result never changes during a transaction.
 */
static inline int ext4_snapshot_is_active(struct inode *inode)
{
	return (inode == EXT4_SB(inode->i_sb)->s_active_snapshot);
}


#define SNAPSHOT_TRANSACTION_ID(sb)				\
	((EXT4_I(EXT4_SB(sb)->s_active_snapshot))->i_datasync_tid)

/**
 * set transaction ID for active snapshot
 *
 * this function is called after freeze_super() returns but before
 * calling unfreeze_super() to record the tid at time when a snapshot is
 * taken.
 */
static inline void ext4_snapshot_set_tid(struct super_block *sb)
{
	BUG_ON(!ext4_snapshot_active(EXT4_SB(sb)));
	SNAPSHOT_TRANSACTION_ID(sb) =
			EXT4_SB(sb)->s_journal->j_transaction_sequence;
}

/* get trancation ID of active snapshot */
static inline tid_t ext4_snapshot_get_tid(struct super_block *sb)
{
	BUG_ON(!ext4_snapshot_active(EXT4_SB(sb)));
	return SNAPSHOT_TRANSACTION_ID(sb);
}

/* test if thereis a mow that is in or before current transcation */
static inline int ext4_snapshot_mow_in_tid(struct inode *inode)
{
	return tid_geq(EXT4_I(inode)->i_datasync_tid,
		      ext4_snapshot_get_tid(inode->i_sb));
}

#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_RACE_COW
/*
 * Pending COW functions
 */

/*
 * Start pending COW operation from get_blocks_handle()
 * after allocating snapshot block and before connecting it
 * to the snapshot inode.
 */
static inline void ext4_snapshot_start_pending_cow(struct buffer_head *sbh)
{
	/*
	 * setting the 'new' flag on a newly allocated snapshot block buffer
	 * indicates that the COW operation is pending.
	 */
	set_buffer_new(sbh);
	/* keep buffer in cache as long as we need to test the 'new' flag */
	get_bh(sbh);
}

/*
 * End pending COW operation started in get_blocks_handle().
 * Called on failure to connect the new snapshot block to the inode
 * or on successful completion of the COW operation.
 */
static inline void ext4_snapshot_end_pending_cow(struct buffer_head *sbh)
{
	/*
	 * clearing the 'new' flag from the snapshot block buffer
	 * indicates that the COW operation is complete.
	 */
	clear_buffer_new(sbh);
	/* we no longer need to keep the buffer in cache */
	put_bh(sbh);
}

/*
 * Test for pending COW operation and wait for its completion.
 */
static inline void ext4_snapshot_test_pending_cow(struct buffer_head *sbh,
						sector_t blocknr)
{
	while (buffer_new(sbh)) {
		/* wait for pending COW to complete */
		snapshot_debug_once(2, "waiting for pending cow: "
				"block = [%llu/%llu]...\n",
				SNAPSHOT_BLOCK_TUPLE(blocknr));
		/*
		 * An unusually long pending COW operation can be caused by
		 * the debugging function snapshot_test_delay(SNAPTEST_COW)
		 * and by waiting for tracked reads to complete.
		 * The new COW buffer is locked during those events, so wait
		 * on the buffer before the short msleep.
		 */
		wait_on_buffer(sbh);
		/*
		 * This is an unlikely event that can happen only once per
		 * block/snapshot, so msleep(1) is sufficient and there is
		 * no need for a wait queue.
		 */
		msleep(1);
		/* XXX: Should we fail after N retries? */
	}
}
#endif
#ifdef CONFIG_EXT4_FS_SNAPSHOT_RACE_READ
/*
 * A tracked reader takes 0x10000 reference counts on the block device buffer.
 * b_count is not likely to reach 0x10000 by get_bh() calls, but even if it
 * does, that will only affect the result of buffer_tracked_readers_count().
 * After 0x10000 subsequent calls to get_bh_tracked_reader(), b_count will
 * overflow, but that requires 0x10000 parallel readers from 0x10000 different
 * snapshots and very slow disk I/O...
 */
#define BH_TRACKED_READERS_COUNT_SHIFT 16

static inline void get_bh_tracked_reader(struct buffer_head *bdev_bh)
{
	atomic_add(1<<BH_TRACKED_READERS_COUNT_SHIFT, &bdev_bh->b_count);
}

static inline void put_bh_tracked_reader(struct buffer_head *bdev_bh)
{
	atomic_sub(1<<BH_TRACKED_READERS_COUNT_SHIFT, &bdev_bh->b_count);
}

static inline int buffer_tracked_readers_count(struct buffer_head *bdev_bh)
{
	return atomic_read(&bdev_bh->b_count)>>BH_TRACKED_READERS_COUNT_SHIFT;
}

/* buffer.c */
extern int start_buffer_tracked_read(struct buffer_head *bh);
extern void cancel_buffer_tracked_read(struct buffer_head *bh);
extern int ext4_read_full_page(struct page *page, get_block_t *get_block);

#ifdef CONFIG_EXT4_DEBUG
extern void __ext4_trace_bh_count(const char *fn, struct buffer_head *bh);
#define ext4_trace_bh_count(bh) __ext4_trace_bh_count(__func__, bh)
#else
#define ext4_trace_bh_count(bh)
#define __ext4_trace_bh_count(fn, bh)
#endif

#define sb_bread(sb, blk) ext4_sb_bread(__func__, sb, blk)
#define sb_getblk(sb, blk) ext4_sb_getblk(__func__, sb, blk)
#define sb_find_get_block(sb, blk) ext4_sb_find_get_block(__func__, sb, blk)

static inline struct buffer_head *
ext4_sb_bread(const char *fn, struct super_block *sb, sector_t block)
{
	struct buffer_head *bh;

	bh = __bread(sb->s_bdev, block, sb->s_blocksize);
	if (bh)
		__ext4_trace_bh_count(fn, bh);
	return bh;
}

static inline struct buffer_head *
ext4_sb_getblk(const char *fn, struct super_block *sb, sector_t block)
{
	struct buffer_head *bh;

	bh = __getblk(sb->s_bdev, block, sb->s_blocksize);
	if (bh)
		__ext4_trace_bh_count(fn, bh);
	return bh;
}

static inline struct buffer_head *
ext4_sb_find_get_block(const char *fn, struct super_block *sb, sector_t block)
{
	struct buffer_head *bh;

	bh = __find_get_block(sb->s_bdev, block, sb->s_blocksize);
	if (bh)
		__ext4_trace_bh_count(fn, bh);
	return bh;
}


#endif

#else /* CONFIG_EXT4_FS_SNAPSHOT */

/* Snapshot NOP macros */
#define EXT4_SNAPSHOTS(sb) (0)
#define SNAPMAP_ISCOW(cmd)	(0)
#define SNAPMAP_ISMOVE(cmd)     (0)
#define SNAPMAP_ISSYNC(cmd)	(0)
#define IS_COWING(handle)	(0)

#define ext4_snapshot_load(sb, es, ro) (0)
#define ext4_snapshot_destroy(sb)
#define init_ext4_snapshot() (0)
#define exit_ext4_snapshot()
#define ext4_snapshot_active(sbi) (0)
#define ext4_snapshot_file(inode) (0)
#define ext4_snapshot_should_move_data(inode) (0)
#define ext4_snapshot_test_excluded(handle, inode, block_to_free, count) (0)
#define ext4_snapshot_list(inode) (0)
#define ext4_snapshot_get_flags(ei, filp)
#define ext4_snapshot_set_flags(handle, inode, flags) (0)
#define ext4_snapshot_take(inode) (0)
#define ext4_snapshot_update(inode_i_sb, cleanup, zero) (0)
#define ext4_snapshot_has_active(sb) (NULL)
#define ext4_snapshot_get_bitmap_access(handle, sb, grp, bh) (0)
#define ext4_snapshot_get_write_access(handle, inode, bh) (0)
#define ext4_snapshot_get_create_access(handle, bh) (0)
#define ext4_snapshot_excluded(ac_inode) (0)
#define ext4_snapshot_get_delete_access(handle, inode, block, pcount) (0)

#define ext4_snapshot_get_move_access(handle, inode, block, pcount, move) (0)
#define ext4_snapshot_start_pending_cow(sbh)
#define ext4_snapshot_end_pending_cow(sbh)
#define ext4_snapshot_is_active(inode)		(0)
#define ext4_snapshot_mow_in_tid(inode)		(1)

#define ext4_snapshot_fix_group_counters(sb, bh, gdp, group)	(0)

#endif /* CONFIG_EXT4_FS_SNAPSHOT */
#endif	/* _LINUX_EXT4_SNAPSHOT_H */
