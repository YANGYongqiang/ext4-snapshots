#ifdef CONFIG_EXT4_FS_AUTO_DEFRAG
/*
 * linux/fs/ext4/auto_defrag.c
 *
 * Written by Yongqiang Yang <xiaoqiangnk@gmail.com>, 2011
 *
 * Copyright (C) 2011 Yongqiang Yang.
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 auto-defag core functions.
 */

/*
 * auto-defrag methodology:
 * For now auto-defrag is designed for ext4 snapshots, so it is based on
 * snaoshot feature.  Metadata is snapshotted by cow while data is
 * snapshotted by mow, mow has less impact on performance, it, however,
 * increases fragment.  Auto-defrag aims to reduce the fragment induced by mow.
 *
 * If the data is rewritten or written first time, auto-defrag looks up if the
 * goal blocks belongs to snapshot, if so, the async reads on goal blocks are
 * issued, then the read end_io call back dirties the blocks.
 *
 * Once the dirtied blocks of snapshot are reallocated and flushed to disk,
 * the original space is freed.  Later the space will be added to the inode's
 * preallocation's space.
 *
 */

/*
 * This function tries to defrag a file by replacing the physical blocks of @ex
 * with the physical blocks of @newex.
 */
int ext4_ext_try_to_defrag(handle_t *handle, struct inode *inode,
			   struct ext4_ext_path *path,
			   struct ext4_map_blocks *map,
			   struct ext4_extent *ex,
			   struct ext4_extent *newex)
{
	ext4_fsblk_t oldblock = 0;
	int err = 0;
	int ee_newlen, merge_len, ee_len, depth;

	depth = ext_depth(inode);
	ee_len = ext4_ext_get_actual_len(ex);
	ee_newlen = ext4_ext_get_actual_len(newex);

	/* determine the nr of blocks which can be replaced */
	merge_len = min(ee_newlen, ee_len);

	BUG_ON(merge_len <= 0 || ee_newlen > ee_len ||
	       newex->ee_block != ex->ee_block);

	if (merge_len == ee_len) {
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto free_blocks;
		ext4_ext_store_pblock(ex, ext4_ext_pblock(newex));
	} else {
		oldblock = ext4_ext_pblock(ex);
		err = ext4_split_extent(handle, inode, path, map, 0,
					EXT4_GET_BLOCKS_PRE_IO);
		if (err < 0)
			goto free_blocks;
		/* extent tree may be changed. */
		depth = ext_depth(inode);
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode, map->m_lblk, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto free_blocks;
		}

		/* just verify splitting. */
		ex = path[depth].p_ext;
		BUG_ON(le32_to_cpu(ex->ee_block) != map->m_lblk ||
		       ext4_ext_get_actual_len(ex) != map->m_len);

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto free_blocks;
		if (!err) {
			/* splice new blocks to the inode*/
			ext4_ext_store_pblock(ex, ext4_ext_pblock(newex));
			ext4_ext_try_to_merge(inode, path, ex);
		}
	}

	if (!err) {
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;
	}

	if (oldblock)
		ext4_free_blocks(handle, inode, NULL, oldblock,
				 merge_len, EXT4_FREE_BLOCKS_FORGET);

	ext4_std_error(inode->i_sb, err);
out:
	return err;

free_blocks:
	ext4_discard_preallocations(inode);
	ext4_free_blocks(handle, inode, NULL, ext4_ext_pblock(newex),
			       ee_newlen, 0);
	return err;
}

/*
 * ext4_map_page_buffers() map buffers attached to @page with blocks stating at
 * @pblk
 */
static void ext4_map_page_buffers(struct page *page, ext4_fsbk_t pblk)
{
	struct inode *inode;
	struct buffer_head *bh;
	struct buffer_head *head;

	BUG_ON(!page->mapping);
 	inode = page->mapping->host;

	if (!page_has_buffers(page))
		create_empty_buffers(page);

	bh = page_buffers(page);
	head = bh;
	do {
		if (unlikely(buffer_mapped(bh)))
			BUG_ON(bh->b_blocknr != pblk);
		else
			map_bh(bh, inode->i_sb, pblk);
		pblk++;
		bh = bh->b_this_page;
	} while (bh != head);

	SetPageMappedToDisk(page);
}

/*
 * set_page_buffers_dirty_remap() sets mapped buffers of
 * the @page dirty and remap and uptodate.
 */
static void set_page_buffers_dirty_remap(struct page *page) {
	struct buffer_head *bh;
	struct buffer_head *head;

	BUG_ON(!page->mapping || !PageUptodate(page) || !page_has_buffers(page));
	
	bh = page_buffers(page);
	head = bh;
	do {
		if (buffer_mapped(bh)) {
			set_buffer_remap(bh);
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
		}
		bh = bh->b_this_page;
	} while(bh != head);
}

/*
 * call back for async bio reads.  This function dirties the buffers.
 */
static void ext4_read_and_dirty_end_io(struct bio *bio, int err) {
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;
	struct buffer_head *bh, *head;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);
		BUG_ON(bio_data_dir(bio) != READ);
		if (uptodate) {
			SetPageUptodate(page);
			set_buffers_dirty_remap(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
}

static struct bio *ext4_read_and_dirty_bio_submit(struct bio *bio)
{
	bio->bi_end_io = ext4_read_and_dirty_end_io;
	submit_bio(READ, bio);
	return NULL;
}

/*
 * I/O completion handler for block_read_full_page() - pages
 * which come unlocked at the end of I/O.
 */
static void end_buffer_async_read_dirty(struct buffer_head *bh, int uptodate)
{
	unsigned long flags;
	struct buffer_head *first;
	struct buffer_head *tmp;
	struct page *page;
	int page_uptodate = 1;

	BUG_ON(!buffer_async_read(bh));

	page = bh->b_page;
	if (uptodate) {
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		set_buffer_remap(bh);
	} else {
		clear_buffer_uptodate(bh);
		if (!quiet_error(bh))
			buffer_io_error(bh);
		SetPageError(page);
	}

	/*
	 * Be _very_ careful from here on. Bad things can happen if
	 * two buffer heads end IO at almost the same time and both
	 * decide that the page is now completely done.
	 */
	first = page_buffers(page);
	local_irq_save(flags);
	bit_spin_lock(BH_Uptodate_Lock, &first->b_state);
	clear_buffer_async_read(bh);
	unlock_buffer(bh);
	tmp = bh;
	do {
		if (!buffer_uptodate(tmp))
			page_uptodate = 0;
		if (buffer_async_read(tmp)) {
			BUG_ON(!buffer_locked(tmp));
			goto still_busy;
		}
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);

	/*
	 * If none of the buffers had errors and they are all
	 * uptodate then we can set the page uptodate.
	 */
	if (page_uptodate && !PageError(page))
		SetPageUptodate(page);
	unlock_page(page);
	return;

still_busy:
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);
	return;
}

static void mark_buffer_async_read_dirty(struct buffer_head *bh)
{
	bh->b_end_io = end_buffer_async_read_dirty;
	set_buffer_async_read(bh);
}

/*
 */
int ext4_read_mapped_buffers_dirty(struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
	int nr, i;

	BUG_ON(!PageLocked(page) || !page_has_buffers(page));

	head = page_buffers(page);
	bh = head;
	nr = 0;
	i = 0;

	do {
		if (buffer_uptodate(bh))
			continue;
		arr[nr++] = bh;
	} while (i++, iblock++, (bh = bh->b_this_page) != head);


	if (!nr) {
		/*
		 * All buffers are uptodate - we can set the page uptodate
		 * as well. But not if get_block() returned an error.
		 */
		if (!PageError(page))
			SetPageUptodate(page);
		unlock_page(page);
		return 0;
	}

	/* Stage two: lock the buffers */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		lock_buffer(bh);
		mark_buffer_async_read(bh);
	}

	/*
	 * Stage 3: start the IO.  Check for uptodateness
	 * inside the buffer lock in case another process reading
	 * the underlying blockdev brought it uptodate (the sct fix).
	 */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		if (buffer_uptodate(bh))
			end_buffer_async_read_redirty(bh, 1);
		else
			submit_bh(READ, bh);
	}
	return 0;
}

/*
 * Copied from mpage_alloc() in fs/mpage.c 
 */
static struct bio *ext4_alloc_bio(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		bio->bi_bdev = bdev;
		bio->bi_sector = first_sector;
	}
	return bio;
}

/*
 * This function issue async reads on requested range and read blocks are
 * dirtied.
 * Simple version of do_mapge_readpage().
 *
 * @page: the page to which the @pblock will be read
 * @nr_pages: the nr of left pages
 * @pblock: the block to be read 
 */
static struct bio *ext4_read_and_dirty_page(struct page *page,
			unsigned nr_pages, ext4_fsblk_t pblock)
{
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	struct block_device *bdev = inode->i_sb->s_bdev;

	if (page_has_buffers(page))
		goto confused;
	ext4_map_page_buffers(page, pblock);

alloc_new:
	if (bio == NULL) {
		bio = ext4_alloc_bio(bdev, pblock << (blkbits - 9),
			  	min_t(int, nr_pages, bio_get_nr_vecs(bdev)),
				GFP_KERNEL);
		if (bio == NULL)
			goto confused;
	}

	if (bio_add_page(bio, page, PAGE_CACHE_SIZE, 0) < length) {
		bio = ext4_read_and_redirty_bio_submit(bio);
		goto alloc_new;
	}

out:
	return bio;

confused:
	if (bio)
		bio = ext4_read_and_dirty_bio_submit(bio);
	if (!PageUptodate(page)) {
		ext4_read_mapped_buffers_dirty(page);
	} else {
		set_buffers_dirty_remap(page);
		unlock_page(page);
	}
	goto out;
}

/*
 * ext4_read_and_dirty_blocks() issues async reads and dirties them
 * in callback.
 *
 * XXX For now this function does not work if blocksize != pagesize
 */
static int ext4_read_and_dirty_blocks(struct inode *inode,
				      struct ext4_ext_map_blocks *map)
{
	int err, i, nr_pages;
	unsigned blkbits = inode->i_blkbits;
	unsigned blks_per_page = 1 << (PAGE_CACHE_SHIFT - blkbits);
	pgoff_t index;
	pgoff_t last_index;
	struct page *page;
	struct page **pages;
	struct bio  *bio = NULL;
	struct buffer_head *bh;
	struct buffer_head *head;
	struct address_space *mapping = inode->i_mapping;
	ext4_lblk_t lblk = map->m_lblk;
	ext4_fsblk_t pblk = map->m_pblk;

	index = lblk >> (PAGE_CACHE_SHIFT - blkbits);
	last_index = (lblk + map->m_len + blks_per_page - 1) >>
		     (PAGE_CACHE_SHIFT - blkbits);
	nr_pages = last_index - index;

	pages = kmalloc(nr_pages * sizeof(*pages));
	if (pages == NULL)
		return -ENOMEM;

	for (i = 0; index < last_index; index++) {
find_page:
		page = find_get_page(mapping, index);
		if (!page) {
			page = page_cache_alloc_cold(mapping);
			if (!page) {
				err = -ENOMEM;
				goto out;
			}

			err = add_to_page_cache_lru(page, mapping,
						    index, GFP_KERNEL);
			if (err) {
				page_cache_release(page);
				if (err = -EEXIST)
					goto find_page;
				goto out;
			}
		} else
			lock_page(page);

		if (PageUptodate(page)) {
			if (PageDirty(page)) {
				unlock_page(page);
				continue;
			}
			ext4_map_page_buffers(page, pblk);
			set_buffers_dirty_remap(page);
			unlock_page(page);
			continue;
		}

		pages[i++] = page;
	}

	for (index = 0; index < i; index++) {
		pblk += pages[index]->index << (PAGE_CACHE_SHIFT - blkbits) - map->m_lblk;
		bio = ext4_read_and_dirty_page(bio, pages[index], i - index,
					       pblk);
		page_cache_release(page);
	}

	if (bio)
		mpage_bio_submit(READ, bio);
out:
	kfree(pages);
	return err;
}

/*
 * This function looks up if the goal blocks belongs to snapshot,
 * if so, async reads on blocks are issued.  The function is used
 * by auto-defrag.
 * On read of snapshot file, an unmapped block is a peephole to prev snapshot.
 *
 * @inode: inode to be defragged.
 * @blk:   the goal block
 * @len:   nr. of blocks.
 */
int ext4_auto_defrag_async_read_blocks(struct inode *inode,
				       ext4_fsblk_t blk,
				       ext4_fsblk_t len)
{
	int err = 0;
	struct ext4_inode_info *ei;
	struct ext4_map_blocks map;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_fsblk_t start = blk;
	ext4_fsblk_t end = blk + len;

	map.m_lblk = SNAPSHOT_IBLOCK(blk);
	map.m_len  = len;
	map.m_flags = 0;

	list_for_each_entry(ei, sbi->s_snapshot_list, i_snapshot_list) {
		struct inode *snapshot;
		snapshot = ei->vfs_inode;

		map.m_lblk = SNAPSHOT_IBLOCK(start);
		map.m_len  = end - start;
		map.m_flags = 0;

		err = ext4_map_blocks(NULL, snapshot, &map, 0);
		if (err < 0)
			return err;
		if (!err)
			continue;
		if (map.m_pblk != SNAPSHOT_BLOCK(map->m_lblk))
			continue;

		err = ext4_read_and_dirty_blocks(snapshot, &map);
		if (err)
			return err;
	}

	return err;
}
#endif
