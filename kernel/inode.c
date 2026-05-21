// SPDX-License-Identifier: GPL-2.0
/*
 * inode.c — операции с inodes и корневой директорией BunkevichFS:
 * формирование имён файлов, iterate_shared, lookup, создание inode.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dcache.h>

#include "bnkfs_internal.h"

/* Формирует имя файла "file_XXXX" по индексу. */
void bnkfs_make_name(struct bnkfs_sb_info *sbi, unsigned int idx,
		     char *out, size_t outsz)
{
	snprintf(out, outsz, "file_%0*u", sbi->name_digits, idx);
}

/* Пытается распарсить "file_NNNN" и вернуть индекс или -1. */
int bnkfs_parse_name(struct bnkfs_sb_info *sbi,
		     const char *name, size_t len)
{
	char buf[BNKFS_NAME_MAX];
	char expected[BNKFS_NAME_MAX];
	unsigned int idx;

	if (len >= sizeof(buf))
		return -1;
	memcpy(buf, name, len);
	buf[len] = '\0';

	if (sscanf(buf, "file_%u", &idx) != 1)
		return -1;
	if (idx >= sbi->file_count)
		return -1;
	bnkfs_make_name(sbi, idx, expected, sizeof(expected));
	if (strcmp(buf, expected) != 0)
		return -1;
	return (int)idx;
}

/* iterate_shared: эмитим синтетические dentry. */
static int bnkfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	char name[BNKFS_NAME_MAX];

	if (!dir_emit_dots(file, ctx))
		return 0;

	while (ctx->pos >= 2 && (ctx->pos - 2) < sbi->file_count) {
		unsigned int idx = (unsigned int)(ctx->pos - 2);

		bnkfs_make_name(sbi, idx, name, sizeof(name));
		if (!dir_emit(ctx, name, strlen(name),
			      BNKFS_FILE_INO_BASE + idx, DT_REG))
			return 0;
		ctx->pos++;
	}
	return 0;
}

/* Lookup в корне: парсим имя и создаём inode на лету. */
static struct dentry *bnkfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	struct inode *inode = NULL;
	int idx;

	if (dentry->d_name.len > sbi->max_name_len)
		return ERR_PTR(-ENAMETOOLONG);

	idx = bnkfs_parse_name(sbi, dentry->d_name.name, dentry->d_name.len);
	if (idx >= 0)
		inode = bnkfs_get_file_inode(sb, (unsigned int)idx);

	return d_splice_alias(inode, dentry);
}

/* Операции корневой директории. */
const struct file_operations bnkfs_dir_fops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = bnkfs_iterate,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = bnkfs_unlocked_ioctl,
};

const struct inode_operations bnkfs_dir_iops = {
	.lookup = bnkfs_lookup,
};

/* ------------------------- создание inode ------------------------- */

const struct inode_operations bnkfs_file_iops = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

struct inode *bnkfs_get_file_inode(struct super_block *sb, unsigned int idx)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	unsigned long ino = BNKFS_FILE_INO_BASE + idx;
	struct inode *inode = iget_locked(sb, ino);

	if (!inode)
		return NULL;
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_mode = S_IFREG | 0644;
	inode->i_uid  = GLOBAL_ROOT_UID;
	inode->i_gid  = GLOBAL_ROOT_GID;
	inode->i_size = (loff_t)sbi->file_sectors * BNKFS_SECTOR_SIZE;
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_current(inode);
	inode->i_op  = &bnkfs_file_iops;
	inode->i_fop = &bnkfs_file_fops;
	unlock_new_inode(inode);
	return inode;
}

struct inode *bnkfs_make_root_inode(struct super_block *sb)
{
	struct inode *inode = iget_locked(sb, BNKFS_ROOT_INO);

	if (!inode)
		return NULL;
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_mode = S_IFDIR | 0755;
	inode->i_uid  = GLOBAL_ROOT_UID;
	inode->i_gid  = GLOBAL_ROOT_GID;
	set_nlink(inode, 2);
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_current(inode);
	inode->i_op  = &bnkfs_dir_iops;
	inode->i_fop = &bnkfs_dir_fops;
	unlock_new_inode(inode);
	return inode;
}
