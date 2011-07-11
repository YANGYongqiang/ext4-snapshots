#ifdef CONFIG_EXT4_FS_AUTO_DEFRAG
/*
 * linux/fs/ext4/auto_defrag.h
 *
 * Written by Yongqiang Yang <xiaoqiangnk@gmail.com>, 2011
 *
 * Copyright (C) 2011 Yongqiang Yang
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext4 auto-defrag extensions.
 */
extern int ext4_ext_try_to_defrag(handle_t *handle, struct inode *inode,
				  struct ext4_ext_path *path,
				  struct ext4_map_blocks *map,
				  struct ext4_extent *ex,
				  struct ext4_extent *newex);

#endif
