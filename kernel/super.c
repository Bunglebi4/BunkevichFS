// SPDX-License-Identifier: GPL-2.0
/*
 * super.c — суперблок BunkevichFS, инициализация модуля, mount/fill_super,
 * чтение/запись копий SB, CRC-проверка целостности и форматирование диска.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <linux/statfs.h>

#include "bnkfs_internal.h"

/* ------------------------- параметры модуля ------------------------- */

static char *disk_name = "";
module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Имя блочного устройства, на котором ожидаем нашу ФС");

static unsigned int sb1_offset = 0;
module_param(sb1_offset, uint, 0444);
MODULE_PARM_DESC(sb1_offset, "Смещение первой копии суперблока в секторах");

static unsigned int sb2_offset = 16;
module_param(sb2_offset, uint, 0444);
MODULE_PARM_DESC(sb2_offset, "Смещение второй копии суперблока в секторах");

static unsigned int max_name_len = 32;
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Максимальная длина имени файла");

static unsigned int max_file_sectors = 4;
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Размер каждого файла в секторах (M)");

/* ------------------------- CRC по структуре SB ------------------------- */

__u32 bnkfs_sb_calc_hash(const struct bnkfs_disk_sb *dsb)
{
	size_t len = offsetof(struct bnkfs_disk_sb, hash);
	return crc32(0, (const u8 *)dsb, len);
}

/* ------------------------- I/O с копиями SB ------------------------- */

/* Записать суперблок в указанный сектор. */
static int bnkfs_write_sb_copy(struct super_block *sb, sector_t where,
			       const struct bnkfs_disk_sb *dsb)
{
	struct buffer_head *bh = sb_bread(sb, where);

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	memcpy(bh->b_data, dsb, sizeof(*dsb));
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/* Прочитать суперблок и проверить целостность хэшем. */
static int bnkfs_read_sb_copy(struct super_block *sb, sector_t where,
			      struct bnkfs_disk_sb *out)
{
	struct buffer_head *bh = sb_bread(sb, where);
	__u32 expected;

	if (!bh)
		return -EIO;
	memcpy(out, bh->b_data, sizeof(*out));
	brelse(bh);

	if (out->magic != BNKFS_MAGIC)
		return -EINVAL;
	expected = bnkfs_sb_calc_hash(out);
	if (expected != out->hash)
		return -EILSEQ; /* хэш не совпал — данные повреждены */
	return 0;
}

/* Форматирование: расставить две копии SB и обнулить файлы. */
static int bnkfs_format_device(struct super_block *sb, struct bnkfs_sb_info *sbi)
{
	struct bnkfs_disk_sb dsb;
	unsigned int i, total;
	int err;

	pr_info("bnkfs: форматирую устройство (file_count=%u, file_sectors=%u)\n",
		sbi->file_count, sbi->file_sectors);

	memset(&dsb, 0, sizeof(dsb));
	dsb.magic             = BNKFS_MAGIC;
	dsb.version           = 1;
	dsb.total_sectors     = sbi->total_sectors;
	dsb.sb1_offset        = sbi->sb1_offset;
	dsb.sb2_offset        = sbi->sb2_offset;
	dsb.data_start_sector = sbi->data_start_sector;
	dsb.file_count        = sbi->file_count;
	dsb.file_sectors      = sbi->file_sectors;
	dsb.max_name_len      = sbi->max_name_len;
	dsb.name_digits       = sbi->name_digits;
	dsb.hash              = bnkfs_sb_calc_hash(&dsb);

	err = bnkfs_write_sb_copy(sb, sbi->sb1_offset, &dsb);
	if (err)
		return err;
	err = bnkfs_write_sb_copy(sb, sbi->sb2_offset, &dsb);
	if (err)
		return err;

	total = sbi->file_count * sbi->file_sectors;
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

int bnkfs_load_or_format(struct super_block *sb, struct bnkfs_sb_info *sbi)
{
	struct bnkfs_disk_sb dsb;
	int err1, err2;

	err1 = bnkfs_read_sb_copy(sb, sbi->sb1_offset, &dsb);
	if (err1 == 0)
		goto loaded;

	pr_warn("bnkfs: основной суперблок повреждён (err=%d), пробую копию\n",
		err1);
	err2 = bnkfs_read_sb_copy(sb, sbi->sb2_offset, &dsb);
	if (err2 == 0) {
		pr_info("bnkfs: восстанавливаю основной суперблок из копии\n");
		bnkfs_write_sb_copy(sb, sbi->sb1_offset, &dsb);
		goto loaded;
	}

	pr_warn("bnkfs: обе копии суперблока невалидны (err1=%d err2=%d), форматирую\n",
		err1, err2);
	return bnkfs_format_device(sb, sbi);

loaded:
	/* Параметры in-memory должны совпадать с тем, что на диске. */
	if (dsb.file_count   != sbi->file_count   ||
	    dsb.file_sectors != sbi->file_sectors ||
	    dsb.sb2_offset   != sbi->sb2_offset) {
		pr_info("bnkfs: параметры ФС изменились, переформатирую\n");
		return bnkfs_format_device(sb, sbi);
	}
	return 0;
}

/* ------------------------- super_operations ------------------------- */

static void bnkfs_put_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static int bnkfs_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);

	buf->f_type    = BNKFS_MAGIC;
	buf->f_bsize   = sb->s_blocksize;
	buf->f_blocks  = sbi->total_sectors;
	buf->f_bfree   = 0;
	buf->f_bavail  = 0;
	buf->f_files   = sbi->file_count;
	buf->f_ffree   = 0;
	buf->f_namelen = sbi->max_name_len;
	return 0;
}

static const struct super_operations bnkfs_sops = {
	.statfs    = bnkfs_statfs,
	.put_super = bnkfs_put_super,
};

/* ------------------------- fill_super ------------------------- */

int bnkfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bnkfs_sb_info *sbi;
	struct inode *root;
	sector_t total_sectors;
	__u32 data_start, max_files;
	int err;

	if (sb1_offset == sb2_offset) {
		pr_err("bnkfs: sb1_offset и sb2_offset должны различаться\n");
		return -EINVAL;
	}
	if (max_file_sectors < 1) {
		pr_err("bnkfs: max_file_sectors должен быть >= 1\n");
		return -EINVAL;
	}
	if (max_name_len < 8 || max_name_len > BNKFS_NAME_MAX - 1) {
		pr_err("bnkfs: max_name_len должен быть в [8 .. %u]\n",
		       BNKFS_NAME_MAX - 1);
		return -EINVAL;
	}

	if (!sb_set_blocksize(sb, BNKFS_BLOCK_SIZE)) {
		pr_err("bnkfs: не удалось установить blocksize=%u\n",
		       BNKFS_BLOCK_SIZE);
		return -EINVAL;
	}

	total_sectors = bdev_nr_sectors(sb->s_bdev);
	if (total_sectors < 16) {
		pr_err("bnkfs: устройство слишком маленькое (%llu секторов)\n",
		       (unsigned long long)total_sectors);
		return -ENOSPC;
	}

	data_start = max(sb1_offset, sb2_offset) + 1;
	if (data_start >= total_sectors) {
		pr_err("bnkfs: смещения суперблоков выходят за пределы устройства\n");
		return -EINVAL;
	}
	max_files = (total_sectors - data_start) / max_file_sectors;
	if (max_files == 0) {
		pr_err("bnkfs: на устройстве не помещается ни одного файла\n");
		return -ENOSPC;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->sb1_offset        = sb1_offset;
	sbi->sb2_offset        = sb2_offset;
	sbi->data_start_sector = data_start;
	sbi->file_count        = max_files;
	sbi->file_sectors      = max_file_sectors;
	sbi->max_name_len      = max_name_len;
	sbi->total_sectors     = total_sectors;

	{
		unsigned int digits = 1, tmp = max_files - 1;

		while (tmp >= 10) { digits++; tmp /= 10; }
		if (digits < 4)
			digits = 4;
		sbi->name_digits = digits;
	}

	sb->s_magic     = BNKFS_MAGIC;
	sb->s_op        = &bnkfs_sops;
	sb->s_fs_info   = sbi;
	sb->s_maxbytes  = (loff_t)sbi->file_sectors * BNKFS_SECTOR_SIZE;
	sb->s_time_gran = 1;

	err = bnkfs_load_or_format(sb, sbi);
	if (err) {
		kfree(sbi);
		sb->s_fs_info = NULL;
		return err;
	}

	root = bnkfs_make_root_inode(sb);
	if (!root) {
		kfree(sbi);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		kfree(sbi);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}

	pr_info("bnkfs: смонтировано: total=%llu, data_start=%u, files=%u x %u секторов\n",
		(unsigned long long)total_sectors, data_start,
		max_files, max_file_sectors);

	if (disk_name && disk_name[0]) {
		const char *devname = sb->s_bdev ?
			sb->s_bdev->bd_disk->disk_name : "";
		if (devname && strcmp(disk_name, devname) != 0 &&
		    !strstr(disk_name, devname))
			pr_warn("bnkfs: mount на %s, а параметр disk_name=%s\n",
				devname, disk_name);
	}
	return 0;
}

/* ------------------------- fs_context API ------------------------- */

static int bnkfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, bnkfs_fill_super);
}

static void bnkfs_free_fc(struct fs_context *fc) { }

static const struct fs_context_operations bnkfs_context_ops = {
	.get_tree = bnkfs_get_tree,
	.free     = bnkfs_free_fc,
};

static int bnkfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &bnkfs_context_ops;
	return 0;
}

static void bnkfs_kill_sb(struct super_block *sb)
{
	if (sb->s_bdev)
		sync_blockdev(sb->s_bdev);
	kill_block_super(sb);
}

static struct file_system_type bnkfs_type = {
	.owner            = THIS_MODULE,
	.name             = BNKFS_FS_NAME,
	.init_fs_context  = bnkfs_init_fs_context,
	.kill_sb          = bnkfs_kill_sb,
	.fs_flags         = FS_REQUIRES_DEV,
};

/* ------------------------- init / exit ------------------------- */

static int __init bnkfs_init(void)
{
	int err = register_filesystem(&bnkfs_type);

	if (err) {
		pr_err("bnkfs: register_filesystem() = %d\n", err);
		return err;
	}
	pr_info("bnkfs: загружен. Параметры: disk_name=%s sb1=%u sb2=%u maxname=%u M=%u\n",
		disk_name, sb1_offset, sb2_offset, max_name_len, max_file_sectors);
	return 0;
}

static void __exit bnkfs_exit(void)
{
	unregister_filesystem(&bnkfs_type);
	pr_info("bnkfs: выгружен\n");
}

module_init(bnkfs_init);
module_exit(bnkfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bunkevich F.S.");
MODULE_DESCRIPTION("BunkevichFS — учебная блочная ФС с двумя копиями суперблока");
MODULE_VERSION("1.0");
