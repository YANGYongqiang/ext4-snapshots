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
#endif
