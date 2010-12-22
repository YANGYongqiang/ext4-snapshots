#
# Makefile for the linux ext4-filesystem routines.
#

obj-$(CONFIG_DEXT4_FS) += dext4.o

dext4-y	:= balloc.o bitmap.o dir.o file.o fsync.o ialloc.o inode.o \
		ioctl.o namei.o super.o symlink.o hash.o resize.o extents.o \
		ext4_jbd2.o migrate.o mballoc.o block_validity.o move_extent.o

dext4-$(CONFIG_DEXT4_FS_XATTR)		+= xattr.o xattr_user.o xattr_trusted.o
dext4-$(CONFIG_DEXT4_FS_POSIX_ACL)	+= acl.o
dext4-$(CONFIG_DEXT4_FS_SECURITY)		+= xattr_security.o
