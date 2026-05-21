// SPDX-License-Identifier: GPL-2.0
/*
 * file.c — операции с файлами BunkevichFS: чтение, запись, fsync и
 * диспетчер IOCTL вместе со всеми его обработчиками.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/string.h>

#include "bnkfs_internal.h"

/* ------------------------- read / write ------------------------- */

static ssize_t bnkfs_file_read(struct file *file, char __user *buf,
			       size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	unsigned int idx = (unsigned int)(inode->i_ino - BNKFS_FILE_INO_BASE);
	loff_t pos = *ppos;
	loff_t size = inode->i_size;
	size_t copied = 0;
	sector_t base;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return 0;
	if (pos + len > size)
		len = size - pos;

	base = bnkfs_file_start_sector(sbi, idx);

	while (copied < len) {
		u64 cur = (u64)pos + copied;
		sector_t sec = base + (sector_t)(cur / BNKFS_SECTOR_SIZE);
		unsigned int off = (unsigned int)(cur % BNKFS_SECTOR_SIZE);
		unsigned int to_copy = min_t(size_t,
					     BNKFS_SECTOR_SIZE - off,
					     len - copied);
		struct buffer_head *bh = sb_bread(sb, sec);

		if (!bh)
			return -EIO;
		if (copy_to_user(buf + copied, bh->b_data + off, to_copy)) {
			brelse(bh);
			return -EFAULT;
		}
		brelse(bh);
		copied += to_copy;
	}
	*ppos = pos + copied;
	return copied;
}

static ssize_t bnkfs_file_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	unsigned int idx = (unsigned int)(inode->i_ino - BNKFS_FILE_INO_BASE);
	loff_t pos = *ppos;
	loff_t size = inode->i_size;
	size_t written = 0;
	sector_t base;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return -ENOSPC; /* файлы фиксированного размера */
	if (pos + len > size)
		len = size - pos;

	base = bnkfs_file_start_sector(sbi, idx);

	while (written < len) {
		u64 cur = (u64)pos + written;
		sector_t sec = base + (sector_t)(cur / BNKFS_SECTOR_SIZE);
		unsigned int off = (unsigned int)(cur % BNKFS_SECTOR_SIZE);
		unsigned int to_copy = min_t(size_t,
					     BNKFS_SECTOR_SIZE - off,
					     len - written);
		struct buffer_head *bh = sb_bread(sb, sec);

		if (!bh)
			return -EIO;
		lock_buffer(bh);
		if (copy_from_user(bh->b_data + off, buf + written, to_copy)) {
			unlock_buffer(bh);
			brelse(bh);
			return -EFAULT;
		}
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
		written += to_copy;
	}
	*ppos = pos + written;
	return written;
}

static int bnkfs_file_fsync(struct file *file, loff_t start, loff_t end,
			    int datasync)
{
	struct super_block *sb = file_inode(file)->i_sb;

	sync_blockdev(sb->s_bdev);
	return 0;
}

/* ------------------------- IOCTL ------------------------- */

static long bnkfs_ioc_zero_all(struct super_block *sb)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	unsigned int total = sbi->file_count * sbi->file_sectors;
	unsigned int i;

	for (i = 0; i < total; i++) {
		sector_t s = sbi->data_start_sector + i;
		struct buffer_head *bh = sb_bread(sb, s);

		if (!bh)
			return -EIO;
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
	}
	sync_blockdev(sb->s_bdev);
	return 0;
}

static long bnkfs_ioc_erase_fs(struct super_block *sb)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	sector_t copies[2] = { sbi->sb1_offset, sbi->sb2_offset };
	int i;

	for (i = 0; i < 2; i++) {
		struct buffer_head *bh = sb_bread(sb, copies[i]);

		if (!bh)
			return -EIO;
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}
	sync_blockdev(sb->s_bdev);
	pr_info("bnkfs: ФС стёрта (обе копии SB обнулены)\n");
	return 0;
}

static int bnkfs_file_hash(struct super_block *sb, unsigned int idx,
			   __u32 *out_hash)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	sector_t base = bnkfs_file_start_sector(sbi, idx);
	unsigned int i;
	__u32 h = 0;

	for (i = 0; i < sbi->file_sectors; i++) {
		struct buffer_head *bh = sb_bread(sb, base + i);

		if (!bh)
			return -EIO;
		h = crc32(h, bh->b_data, BNKFS_SECTOR_SIZE);
		brelse(bh);
	}
	*out_hash = h;
	return 0;
}

static long bnkfs_ioc_get_hashes(struct super_block *sb, void __user *argp)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	struct bnkfs_hashes_req req;
	struct bnkfs_file_meta *entries;
	unsigned int i, to_write;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	to_write = min_t(unsigned int, req.max_count, sbi->file_count);
	entries = NULL;
	if (to_write) {
		entries = kzalloc(sizeof(*entries) * to_write, GFP_KERNEL);
		if (!entries)
			return -ENOMEM;
	}

	for (i = 0; i < to_write; i++) {
		bnkfs_make_name(sbi, i, entries[i].name, sizeof(entries[i].name));
		entries[i].start_sector = bnkfs_file_start_sector(sbi, i);
		entries[i].sector_count = sbi->file_sectors;
		ret = bnkfs_file_hash(sb, i, &entries[i].hash);
		if (ret)
			goto out;
	}

	if (to_write && copy_to_user((void __user *)(uintptr_t)req.entries,
				     entries, sizeof(*entries) * to_write)) {
		ret = -EFAULT;
		goto out;
	}

	req.count = sbi->file_count;
	if (copy_to_user(argp, &req, sizeof(req)))
		ret = -EFAULT;

out:
	kfree(entries);
	return ret;
}

static long bnkfs_ioc_get_mapping(struct super_block *sb, void __user *argp)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	struct bnkfs_mapping req;
	int idx;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.name[BNKFS_NAME_MAX - 1] = '\0';

	idx = bnkfs_parse_name(sbi, req.name,
			       strnlen(req.name, BNKFS_NAME_MAX));
	if (idx < 0)
		return -ENOENT;

	req.start_sector = bnkfs_file_start_sector(sbi, idx);
	req.sector_count = sbi->file_sectors;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

long bnkfs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case BNKFS_IOC_ZERO_ALL:
		return bnkfs_ioc_zero_all(sb);
	case BNKFS_IOC_ERASE_FS:
		return bnkfs_ioc_erase_fs(sb);
	case BNKFS_IOC_GET_HASHES:
		return bnkfs_ioc_get_hashes(sb, argp);
	case BNKFS_IOC_GET_MAPPING:
		return bnkfs_ioc_get_mapping(sb, argp);
	default:
		return -ENOTTY;
	}
}

/* ------------------------- file_operations ------------------------- */

const struct file_operations bnkfs_file_fops = {
	.owner          = THIS_MODULE,
	.read           = bnkfs_file_read,
	.write          = bnkfs_file_write,
	.llseek         = generic_file_llseek,
	.fsync          = bnkfs_file_fsync,
	.unlocked_ioctl = bnkfs_unlocked_ioctl,
};
