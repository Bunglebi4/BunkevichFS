// SPDX-License-Identifier: GPL-2.0
/*
 * BunkevichFS — простая ФС поверх блочного устройства.
 *
 * Архитектура:
 *   sector 0..               : superblock (копия 1)
 *   sector sb2_offset        : superblock (копия 2)
 *   первый свободный сектор  : файлы фиксированного размера M, подряд,
 *                              пропуская сектор резервной копии SB.
 *
 * Поддерживаются read/write обычных файлов и 4 IOCTL.
 * Версия ядра: 6.12.x.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>

#include "bunglefs.h"

#define BUNGLEFS_FS_NAME "bunglefs"

/* ---------------- module parameters ---------------- */
static char  *disk_name        = "/dev/loop0";
static uint   sb1_offset       = 0;
static uint   sb2_offset       = 1024;
static uint   max_name_len     = BUNGLEFS_NAME_LEN;
static uint   max_file_sectors = 4; /* M */

module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Path to backing block device");
module_param(sb1_offset, uint, 0444);
MODULE_PARM_DESC(sb1_offset, "Sector offset of primary superblock (default 0)");
module_param(sb2_offset, uint, 0444);
MODULE_PARM_DESC(sb2_offset, "Sector offset of backup superblock");
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Max filename length (<= BUNGLEFS_NAME_LEN)");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Max file size in sectors (M, 1..)");

/* ---------------- in-memory structures ---------------- */
struct bunglefs_fs_info {
	struct file              *bdev_file;
	struct block_device      *bdev;
	struct bunglefs_dsb      *dsb;
	struct mutex              lock;
};

/* Helpers -------------------------------------------------- */

static __u32 dsb_compute_hash(const struct bunglefs_dsb *dsb)
{
	__u32 saved = le32_to_cpu(dsb->hash);
	__u32 crc;
	struct bunglefs_dsb *tmp;

	tmp = kmalloc(sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return 0;
	memcpy(tmp, dsb, sizeof(*tmp));
	tmp->hash = 0;
	crc = crc32_le(0, (const u8 *)tmp, sizeof(*tmp));
	kfree(tmp);
	(void)saved;
	return crc;
}

static int read_sector(struct block_device *bdev, sector_t sec, void *buf)
{
	struct buffer_head *bh;

	bh = __bread(bdev, sec, BUNGLEFS_SECTOR_SIZE);
	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, BUNGLEFS_SECTOR_SIZE);
	brelse(bh);
	return 0;
}

static int write_sector(struct block_device *bdev, sector_t sec, const void *buf)
{
	struct buffer_head *bh;

	bh = __getblk(bdev, sec, BUNGLEFS_SECTOR_SIZE);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, BUNGLEFS_SECTOR_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/* DSB occupies more than one sector — write each sector individually. */
static int write_dsb_at(struct block_device *bdev, sector_t base,
			const struct bunglefs_dsb *dsb)
{
	const u8 *p = (const u8 *)dsb;
	size_t total = sizeof(*dsb);
	size_t off = 0;
	int rc;

	while (off < total) {
		size_t chunk = min_t(size_t, BUNGLEFS_SECTOR_SIZE, total - off);
		u8 sec[BUNGLEFS_SECTOR_SIZE] = {0};

		memcpy(sec, p + off, chunk);
		rc = write_sector(bdev, base + (off / BUNGLEFS_SECTOR_SIZE), sec);
		if (rc)
			return rc;
		off += BUNGLEFS_SECTOR_SIZE;
	}
	return 0;
}

static int read_dsb_at(struct block_device *bdev, sector_t base,
		       struct bunglefs_dsb *dsb)
{
	u8 *p = (u8 *)dsb;
	size_t total = sizeof(*dsb);
	size_t off = 0;
	int rc;

	while (off < total) {
		u8 sec[BUNGLEFS_SECTOR_SIZE];

		rc = read_sector(bdev, base + (off / BUNGLEFS_SECTOR_SIZE), sec);
		if (rc)
			return rc;
		memcpy(p + off,
		       sec,
		       min_t(size_t, BUNGLEFS_SECTOR_SIZE, total - off));
		off += BUNGLEFS_SECTOR_SIZE;
	}
	return 0;
}

static unsigned int dsb_sector_span(void)
{
	return (sizeof(struct bunglefs_dsb) + BUNGLEFS_SECTOR_SIZE - 1) /
	       BUNGLEFS_SECTOR_SIZE;
}

/* Verify and load SB: read primary, check hash; if bad, fall back to backup. */
static int load_or_init_dsb(struct bunglefs_fs_info *fsi)
{
	struct bunglefs_dsb *dsb1, *dsb2;
	int rc = 0;
	bool ok1 = false, ok2 = false;

	dsb1 = kzalloc(sizeof(*dsb1), GFP_KERNEL);
	dsb2 = kzalloc(sizeof(*dsb2), GFP_KERNEL);
	if (!dsb1 || !dsb2) {
		rc = -ENOMEM;
		goto out;
	}

	if (!read_dsb_at(fsi->bdev, sb1_offset, dsb1) &&
	    le32_to_cpu(dsb1->magic) == BUNGLEFS_MAGIC &&
	    dsb_compute_hash(dsb1) == le32_to_cpu(dsb1->hash))
		ok1 = true;

	if (!read_dsb_at(fsi->bdev, sb2_offset, dsb2) &&
	    le32_to_cpu(dsb2->magic) == BUNGLEFS_MAGIC &&
	    dsb_compute_hash(dsb2) == le32_to_cpu(dsb2->hash))
		ok2 = true;

	if (ok1) {
		memcpy(fsi->dsb, dsb1, sizeof(*dsb1));
		pr_info("bunglefs: primary superblock OK\n");
		if (!ok2) {
			pr_warn("bunglefs: backup SB invalid, rewriting\n");
			fsi->dsb->hash = cpu_to_le32(dsb_compute_hash(fsi->dsb));
			write_dsb_at(fsi->bdev, sb2_offset, fsi->dsb);
		}
	} else if (ok2) {
		memcpy(fsi->dsb, dsb2, sizeof(*dsb2));
		pr_warn("bunglefs: primary SB invalid, restored from backup\n");
		fsi->dsb->hash = cpu_to_le32(dsb_compute_hash(fsi->dsb));
		write_dsb_at(fsi->bdev, sb1_offset, fsi->dsb);
	} else {
		/* Format fresh. */
		struct bunglefs_dsb *d = fsi->dsb;
		sector_t total = bdev_nr_sectors(fsi->bdev);
		unsigned int span = dsb_sector_span();
		sector_t first_file;
		unsigned int i, n;
		sector_t cursor;

		pr_info("bunglefs: no valid superblock found — formatting\n");

		memset(d, 0, sizeof(*d));
		d->magic            = cpu_to_le32(BUNGLEFS_MAGIC);
		d->file_size_sectors = cpu_to_le32(max_file_sectors);
		d->max_name_len     = cpu_to_le32(min_t(uint, max_name_len, BUNGLEFS_NAME_LEN));
		d->sb1_offset       = cpu_to_le32(sb1_offset);
		d->sb2_offset       = cpu_to_le32(sb2_offset);
		d->total_sectors    = cpu_to_le32((u32)total);

		/* Lay out files after SB1, skipping SB2 region. */
		first_file = sb1_offset + span;
		d->first_file_sector = cpu_to_le32((u32)first_file);

		cursor = first_file;
		for (i = 0, n = 0; n < BUNGLEFS_MAX_FILES && i < BUNGLEFS_MAX_FILES; i++) {
			/* Skip over SB2 area if we hit it. */
			if (cursor >= sb2_offset && cursor < sb2_offset + span)
				cursor = sb2_offset + span;
			if (cursor + max_file_sectors > total)
				break;
			snprintf(d->files[n].name, BUNGLEFS_NAME_LEN, "file%u", n);
			d->files[n].start_sector  = cpu_to_le32((u32)cursor);
			d->files[n].size_sectors  = cpu_to_le32(max_file_sectors);
			d->files[n].content_hash  = 0;
			cursor += max_file_sectors;
			n++;
		}
		d->file_count = cpu_to_le32(n);
		d->hash       = cpu_to_le32(dsb_compute_hash(d));

		rc = write_dsb_at(fsi->bdev, sb1_offset, d);
		if (rc) goto out;
		rc = write_dsb_at(fsi->bdev, sb2_offset, d);
		if (rc) goto out;

		pr_info("bunglefs: formatted, %u files, M=%u sectors\n",
			n, max_file_sectors);
	}

out:
	kfree(dsb1);
	kfree(dsb2);
	return rc;
}

static int persist_dsb(struct bunglefs_fs_info *fsi)
{
	int rc;

	fsi->dsb->hash = 0;
	fsi->dsb->hash = cpu_to_le32(dsb_compute_hash(fsi->dsb));
	rc = write_dsb_at(fsi->bdev, le32_to_cpu(fsi->dsb->sb1_offset), fsi->dsb);
	if (rc)
		return rc;
	return write_dsb_at(fsi->bdev, le32_to_cpu(fsi->dsb->sb2_offset), fsi->dsb);
}

/* ---------------- file I/O ---------------- */

static int file_index_from_inode(struct inode *inode)
{
	return (int)(inode->i_ino - 2); /* root is ino 1, files start at 2 */
}

static loff_t file_max_bytes(struct bunglefs_fs_info *fsi, int idx)
{
	return (loff_t)le32_to_cpu(fsi->dsb->files[idx].size_sectors) *
	       BUNGLEFS_SECTOR_SIZE;
}

static int file_read_all(struct bunglefs_fs_info *fsi, int idx, u8 *buf)
{
	u32 start = le32_to_cpu(fsi->dsb->files[idx].start_sector);
	u32 sz    = le32_to_cpu(fsi->dsb->files[idx].size_sectors);
	u32 i;
	int rc;

	for (i = 0; i < sz; i++) {
		rc = read_sector(fsi->bdev, start + i,
				 buf + i * BUNGLEFS_SECTOR_SIZE);
		if (rc)
			return rc;
	}
	return 0;
}

static int file_write_all(struct bunglefs_fs_info *fsi, int idx, const u8 *buf)
{
	u32 start = le32_to_cpu(fsi->dsb->files[idx].start_sector);
	u32 sz    = le32_to_cpu(fsi->dsb->files[idx].size_sectors);
	u32 i;
	int rc;

	for (i = 0; i < sz; i++) {
		rc = write_sector(fsi->bdev, start + i,
				  buf + i * BUNGLEFS_SECTOR_SIZE);
		if (rc)
			return rc;
	}
	fsi->dsb->files[idx].content_hash =
		cpu_to_le32(crc32_le(0, buf, sz * BUNGLEFS_SECTOR_SIZE));
	return persist_dsb(fsi);
}

static ssize_t bunglefs_read(struct file *filp, char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct bunglefs_fs_info *fsi = inode->i_sb->s_fs_info;
	int idx = file_index_from_inode(inode);
	loff_t maxb;
	u8 *buf;
	ssize_t rc;

	if (idx < 0 || idx >= (int)le32_to_cpu(fsi->dsb->file_count))
		return -EINVAL;
	maxb = file_max_bytes(fsi, idx);
	if (*ppos >= maxb)
		return 0;
	if (*ppos + len > maxb)
		len = maxb - *ppos;

	buf = kvmalloc(maxb, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&fsi->lock);
	rc = file_read_all(fsi, idx, buf);
	mutex_unlock(&fsi->lock);
	if (rc) {
		kvfree(buf);
		return rc;
	}
	if (copy_to_user(ubuf, buf + *ppos, len)) {
		kvfree(buf);
		return -EFAULT;
	}
	kvfree(buf);
	*ppos += len;
	return len;
}

static ssize_t bunglefs_write(struct file *filp, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct bunglefs_fs_info *fsi = inode->i_sb->s_fs_info;
	int idx = file_index_from_inode(inode);
	loff_t maxb;
	u8 *buf;
	ssize_t rc;

	if (idx < 0 || idx >= (int)le32_to_cpu(fsi->dsb->file_count))
		return -EINVAL;
	maxb = file_max_bytes(fsi, idx);
	if (*ppos >= maxb)
		return -ENOSPC;
	if (*ppos + len > maxb)
		len = maxb - *ppos;

	buf = kvmalloc(maxb, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&fsi->lock);
	rc = file_read_all(fsi, idx, buf);
	if (rc) {
		mutex_unlock(&fsi->lock);
		kvfree(buf);
		return rc;
	}
	if (copy_from_user(buf + *ppos, ubuf, len)) {
		mutex_unlock(&fsi->lock);
		kvfree(buf);
		return -EFAULT;
	}
	rc = file_write_all(fsi, idx, buf);
	mutex_unlock(&fsi->lock);
	kvfree(buf);
	if (rc)
		return rc;
	*ppos += len;
	i_size_write(inode, max_t(loff_t, i_size_read(inode), *ppos));
	return len;
}

/* ---------------- IOCTL ---------------- */

static long bunglefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct bunglefs_fs_info *fsi = inode->i_sb->s_fs_info;
	void __user *uarg = (void __user *)arg;
	long rc = 0;

	switch (cmd) {
	case BUNGLEFS_IOC_ZERO_ALL: {
		u32 n = le32_to_cpu(fsi->dsb->file_count);
		u32 i;
		u8 *zero;
		loff_t maxb;

		mutex_lock(&fsi->lock);
		maxb = (loff_t)max_file_sectors * BUNGLEFS_SECTOR_SIZE;
		zero = kvzalloc(maxb, GFP_KERNEL);
		if (!zero) {
			mutex_unlock(&fsi->lock);
			return -ENOMEM;
		}
		for (i = 0; i < n; i++) {
			rc = file_write_all(fsi, i, zero);
			if (rc)
				break;
		}
		kvfree(zero);
		mutex_unlock(&fsi->lock);
		break;
	}
	case BUNGLEFS_IOC_WIPE_FS: {
		u8 sec[BUNGLEFS_SECTOR_SIZE] = {0};
		unsigned int span = dsb_sector_span();
		unsigned int i;

		mutex_lock(&fsi->lock);
		for (i = 0; i < span; i++) {
			write_sector(fsi->bdev,
				     le32_to_cpu(fsi->dsb->sb1_offset) + i, sec);
			write_sector(fsi->bdev,
				     le32_to_cpu(fsi->dsb->sb2_offset) + i, sec);
		}
		memset(fsi->dsb, 0, sizeof(*fsi->dsb));
		mutex_unlock(&fsi->lock);
		pr_warn("bunglefs: filesystem wiped\n");
		break;
	}
	case BUNGLEFS_IOC_LIST_META: {
		struct bunglefs_meta_list *list;
		u32 n, i;

		list = kzalloc(sizeof(*list), GFP_KERNEL);
		if (!list)
			return -ENOMEM;
		mutex_lock(&fsi->lock);
		n = le32_to_cpu(fsi->dsb->file_count);
		list->count = n;
		for (i = 0; i < n; i++) {
			memcpy(list->entries[i].name,
			       fsi->dsb->files[i].name, BUNGLEFS_NAME_LEN);
			list->entries[i].start_sector =
				le32_to_cpu(fsi->dsb->files[i].start_sector);
			list->entries[i].size_sectors =
				le32_to_cpu(fsi->dsb->files[i].size_sectors);
			list->entries[i].content_hash =
				le32_to_cpu(fsi->dsb->files[i].content_hash);
		}
		mutex_unlock(&fsi->lock);
		if (copy_to_user(uarg, list, sizeof(*list)))
			rc = -EFAULT;
		kfree(list);
		break;
	}
	case BUNGLEFS_IOC_FILE_MAP: {
		struct bunglefs_file_map fm;
		u32 i, n;

		if (copy_from_user(&fm, uarg, sizeof(fm)))
			return -EFAULT;
		fm.found = 0;
		mutex_lock(&fsi->lock);
		n = le32_to_cpu(fsi->dsb->file_count);
		for (i = 0; i < n; i++) {
			if (strncmp(fm.name, fsi->dsb->files[i].name,
				    BUNGLEFS_NAME_LEN) == 0) {
				fm.start_sector =
					le32_to_cpu(fsi->dsb->files[i].start_sector);
				fm.size_sectors =
					le32_to_cpu(fsi->dsb->files[i].size_sectors);
				fm.found = 1;
				break;
			}
		}
		mutex_unlock(&fsi->lock);
		if (copy_to_user(uarg, &fm, sizeof(fm)))
			rc = -EFAULT;
		break;
	}
	default:
		rc = -ENOTTY;
	}
	return rc;
}

static const struct file_operations bunglefs_file_ops = {
	.owner          = THIS_MODULE,
	.read           = bunglefs_read,
	.write          = bunglefs_write,
	.unlocked_ioctl = bunglefs_ioctl,
	.compat_ioctl   = bunglefs_ioctl,
	.llseek         = default_llseek,
};

/* ---------------- directory ops ---------------- */

static int bunglefs_readdir(struct file *filp, struct dir_context *ctx)
{
	struct inode *inode = file_inode(filp);
	struct bunglefs_fs_info *fsi = inode->i_sb->s_fs_info;
	u32 n = le32_to_cpu(fsi->dsb->file_count);

	if (!dir_emit_dots(filp, ctx))
		return 0;
	while (ctx->pos - 2 < n) {
		u32 i = ctx->pos - 2;
		const char *name = fsi->dsb->files[i].name;
		size_t nlen = strnlen(name, BUNGLEFS_NAME_LEN);

		if (!dir_emit(ctx, name, nlen, 2 + i, DT_REG))
			break;
		ctx->pos++;
	}
	return 0;
}

static const struct file_operations bunglefs_dir_ops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = bunglefs_readdir,
	.unlocked_ioctl = bunglefs_ioctl,
	.compat_ioctl   = bunglefs_ioctl,
	.llseek         = default_llseek,
};

/* ---------------- inode / lookup ---------------- */

static struct inode *bunglefs_iget(struct super_block *sb, unsigned long ino,
				   umode_t mode, loff_t size)
{
	struct inode *inode = iget_locked(sb, ino);

	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_mode  = mode;
	inode->i_uid   = current_fsuid();
	inode->i_gid   = current_fsgid();
	simple_inode_init_ts(inode);
	i_size_write(inode, size);

	if (S_ISDIR(mode)) {
		inode->i_op   = &simple_dir_inode_operations;
		inode->i_fop  = &bunglefs_dir_ops;
		inc_nlink(inode);
	} else {
		inode->i_fop  = &bunglefs_file_ops;
	}
	unlock_new_inode(inode);
	return inode;
}

static struct dentry *bunglefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct bunglefs_fs_info *fsi = dir->i_sb->s_fs_info;
	const char *name = dentry->d_name.name;
	u32 i, n = le32_to_cpu(fsi->dsb->file_count);

	for (i = 0; i < n; i++) {
		if (strncmp(name, fsi->dsb->files[i].name, BUNGLEFS_NAME_LEN) == 0) {
			loff_t sz = (loff_t)le32_to_cpu(fsi->dsb->files[i].size_sectors)
				    * BUNGLEFS_SECTOR_SIZE;
			struct inode *ino = bunglefs_iget(dir->i_sb, 2 + i,
							  S_IFREG | 0644, sz);
			if (IS_ERR(ino))
				return ERR_CAST(ino);
			return d_splice_alias(ino, dentry);
		}
	}
	return d_splice_alias(NULL, dentry);
}

static const struct inode_operations bunglefs_dir_inode_ops = {
	.lookup = bunglefs_lookup,
};

/* ---------------- super_operations ---------------- */

static void bunglefs_put_super(struct super_block *sb)
{
	struct bunglefs_fs_info *fsi = sb->s_fs_info;

	if (!fsi)
		return;
	if (fsi->bdev_file)
		fput(fsi->bdev_file);
	kfree(fsi->dsb);
	kfree(fsi);
	sb->s_fs_info = NULL;
}

static int bunglefs_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct bunglefs_fs_info *fsi = d->d_sb->s_fs_info;

	buf->f_type    = BUNGLEFS_MAGIC;
	buf->f_bsize   = BUNGLEFS_SECTOR_SIZE;
	buf->f_blocks  = le32_to_cpu(fsi->dsb->total_sectors);
	buf->f_files   = le32_to_cpu(fsi->dsb->file_count);
	buf->f_namelen = BUNGLEFS_NAME_LEN;
	return 0;
}

static const struct super_operations bunglefs_sops = {
	.put_super   = bunglefs_put_super,
	.statfs      = bunglefs_statfs,
	.drop_inode  = generic_delete_inode,
};

/* ---------------- mount / fs_context ---------------- */

static int bunglefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bunglefs_fs_info *fsi;
	struct inode *root;
	int rc;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;
	mutex_init(&fsi->lock);

	fsi->bdev_file = bdev_file_open_by_path(disk_name,
				BLK_OPEN_READ | BLK_OPEN_WRITE,
				sb, NULL);
	if (IS_ERR(fsi->bdev_file)) {
		rc = PTR_ERR(fsi->bdev_file);
		fsi->bdev_file = NULL;
		pr_err("bunglefs: cannot open %s (%d)\n", disk_name, rc);
		goto err;
	}
	fsi->bdev = file_bdev(fsi->bdev_file);

	fsi->dsb = kzalloc(sizeof(*fsi->dsb), GFP_KERNEL);
	if (!fsi->dsb) {
		rc = -ENOMEM;
		goto err;
	}

	sb->s_magic     = BUNGLEFS_MAGIC;
	sb->s_blocksize = BUNGLEFS_SECTOR_SIZE;
	sb->s_blocksize_bits = ilog2(BUNGLEFS_SECTOR_SIZE);
	sb->s_maxbytes  = (loff_t)max_file_sectors * BUNGLEFS_SECTOR_SIZE;
	sb->s_op        = &bunglefs_sops;
	sb->s_fs_info   = fsi;
	sb->s_time_gran = 1;

	/* set s_bdev so sb_bread() helpers work if needed elsewhere */
	sb->s_bdev = fsi->bdev;

	rc = load_or_init_dsb(fsi);
	if (rc)
		goto err;

	root = bunglefs_iget(sb, 1, S_IFDIR | 0755, 0);
	if (IS_ERR(root)) {
		rc = PTR_ERR(root);
		goto err;
	}
	root->i_op = &bunglefs_dir_inode_ops;

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		rc = -ENOMEM;
		goto err;
	}
	pr_info("bunglefs: mounted on %s, %u files\n",
		disk_name, le32_to_cpu(fsi->dsb->file_count));
	return 0;

err:
	if (fsi->bdev_file)
		fput(fsi->bdev_file);
	kfree(fsi->dsb);
	kfree(fsi);
	sb->s_fs_info = NULL;
	return rc;
}

static int bunglefs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, bunglefs_fill_super);
}

static const struct fs_context_operations bunglefs_ctx_ops = {
	.get_tree = bunglefs_get_tree,
};

static int bunglefs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &bunglefs_ctx_ops;
	return 0;
}

static void bunglefs_kill_sb(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type bunglefs_type = {
	.owner            = THIS_MODULE,
	.name             = BUNGLEFS_FS_NAME,
	.init_fs_context  = bunglefs_init_fs_context,
	.kill_sb          = bunglefs_kill_sb,
	.fs_flags         = 0,
};

/* ---------------- init/exit ---------------- */

static int __init bunglefs_mod_init(void)
{
	int rc;

	if (max_name_len == 0 || max_name_len > BUNGLEFS_NAME_LEN)
		max_name_len = BUNGLEFS_NAME_LEN;
	if (max_file_sectors == 0)
		max_file_sectors = 1;
	if (sb2_offset == sb1_offset) {
		pr_err("bunglefs: sb1_offset == sb2_offset\n");
		return -EINVAL;
	}

	rc = register_filesystem(&bunglefs_type);
	if (rc) {
		pr_err("bunglefs: register_filesystem failed: %d\n", rc);
		return rc;
	}
	pr_info("bunglefs: loaded (disk=%s sb1=%u sb2=%u M=%u name=%u)\n",
		disk_name, sb1_offset, sb2_offset, max_file_sectors, max_name_len);
	return 0;
}

static void __exit bunglefs_mod_exit(void)
{
	unregister_filesystem(&bunglefs_type);
	pr_info("bunglefs: unloaded\n");
}

module_init(bunglefs_mod_init);
module_exit(bunglefs_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bunkevich F.S.");
MODULE_DESCRIPTION("SimpleFS / BunkevichFS — учебная ФС с двумя копиями SB");
MODULE_VERSION("0.1");
