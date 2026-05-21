// SPDX-License-Identifier: GPL-2.0
/*
 * BunkevichFS — учебная файловая система для ядра Linux 6.12.
 *
 * Особенности:
 *  - Размещается поверх существующего блочного устройства.
 *  - Содержит две копии суперблока: в секторе 0 и в секторе sb2_offset.
 *  - Целостность суперблока контролируется CRC32 по всем полям, кроме самого хэша.
 *  - При инициализации (первый mount) все файлы создаются заранее:
 *    каждый файл занимает ровно max_file_sectors (M) последовательных секторов.
 *  - Имена файлов формируются автоматически: "file_0000", "file_0001", ...
 *  - Поддерживаются только чтение и запись содержимого файлов плюс несколько IOCTL.
 *
 * Параметры модуля:
 *  - disk_name           — имя ожидаемого блочного устройства (для проверки при mount).
 *  - sb1_offset          — смещение первой копии суперблока в секторах (обычно 0).
 *  - sb2_offset          — смещение второй копии суперблока в секторах.
 *  - max_name_len        — максимальная длина имени файла.
 *  - max_file_sectors    — размер каждого файла в секторах (параметр M).
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
#include <linux/uaccess.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>
#include <linux/version.h>

#include "bnkfs_ioctl.h"

#define BNKFS_MAGIC        0x424E4B46u /* "BNKF" */
#define BNKFS_SECTOR_SIZE  512u
#define BNKFS_BLOCK_SIZE   BNKFS_SECTOR_SIZE /* работаем секторами 512 байт */
#define BNKFS_ROOT_INO     1u
#define BNKFS_FILE_INO_BASE 2u /* первый файловый inode */

/* ------------------------------------------------------------- */
/*                       Параметры модуля                          */
/* ------------------------------------------------------------- */

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

/* ------------------------------------------------------------- */
/*                       Дисковые структуры                        */
/* ------------------------------------------------------------- */

/*
 * Дисковый суперблок. Размещается в одном секторе, поэтому ограничен 512 байт.
 * Все поля little-endian — храним «как есть» (для учебного модуля этого достаточно).
 */
struct bnkfs_disk_sb {
	__u32 magic;             /* BNKFS_MAGIC */
	__u32 version;           /* версия формата (1) */
	__u32 total_sectors;     /* общее число секторов на устройстве */
	__u32 sb1_offset;        /* положение первой копии суперблока */
	__u32 sb2_offset;        /* положение второй копии суперблока */
	__u32 data_start_sector; /* первый сектор с данными файлов */
	__u32 file_count;        /* число файлов в ФС */
	__u32 file_sectors;      /* сколько секторов занимает каждый файл (M) */
	__u32 max_name_len;      /* максимальная длина имени файла */
	__u32 name_digits;       /* сколько цифр используется в имени файла */
	__u32 reserved[21];      /* выравнивание и место под будущее */
	__u32 hash;              /* CRC32 по всем полям выше */
} __packed;

/* Считаем CRC32 по структуре, кроме поля hash в самом конце. */
static __u32 bnkfs_sb_calc_hash(const struct bnkfs_disk_sb *dsb)
{
	size_t len = offsetof(struct bnkfs_disk_sb, hash);
	return crc32(0, (const u8 *)dsb, len);
}

/* ------------------------------------------------------------- */
/*                    In-memory информация о ФС                    */
/* ------------------------------------------------------------- */

struct bnkfs_sb_info {
	__u32 sb1_offset;
	__u32 sb2_offset;
	__u32 data_start_sector;
	__u32 file_count;
	__u32 file_sectors;
	__u32 max_name_len;
	__u32 name_digits;
	__u32 total_sectors;
};

/* По индексу файла возвращаем стартовый сектор. */
static inline sector_t bnkfs_file_start_sector(struct bnkfs_sb_info *sbi, unsigned int idx)
{
	return (sector_t)sbi->data_start_sector + (sector_t)idx * sbi->file_sectors;
}

/* Формирует строковое имя файла по индексу (file_XXXX). */
static void bnkfs_make_name(struct bnkfs_sb_info *sbi, unsigned int idx, char *out, size_t outsz)
{
	snprintf(out, outsz, "file_%0*u", sbi->name_digits, idx);
}

/* Пытается распарсить имя "file_NNNN" и вернуть индекс или -1. */
static int bnkfs_parse_name(struct bnkfs_sb_info *sbi, const char *name, size_t len)
{
	char buf[BNKFS_NAME_MAX];
	unsigned int idx;
	char expected[BNKFS_NAME_MAX];

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

/* ------------------------------------------------------------- */
/*                  Forward declarations                           */
/* ------------------------------------------------------------- */

static const struct super_operations bnkfs_sops;
static const struct inode_operations bnkfs_dir_iops;
static const struct file_operations  bnkfs_dir_fops;
static const struct inode_operations bnkfs_file_iops;
static const struct file_operations  bnkfs_file_fops;

/* ------------------------------------------------------------- */
/*                     Операции с суперблоком                      */
/* ------------------------------------------------------------- */

/* Записать суперблок в указанный сектор (через буферный кэш). */
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

/* Прочитать суперблок из указанного сектора и проверить хэш. */
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
		return -EILSEQ; /* хэш не совпал → данные повреждены */
	return 0;
}

/*
 * Создать ФС на устройстве с нуля: расставить две копии суперблока и обнулить
 * сектора всех файлов. Вызывается, если ни одна копия SB не прочиталась корректно.
 */
static int bnkfs_format_device(struct super_block *sb, struct bnkfs_sb_info *sbi)
{
	struct bnkfs_disk_sb dsb;
	unsigned int i;
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

	/* Обнуляем сектора всех файлов, чтобы их содержимое было предсказуемо. */
	for (i = 0; i < sbi->file_count * sbi->file_sectors; i++) {
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

/*
 * Прочитать и провалидировать суперблок: сначала пробуем основной, при ошибке —
 * запасной. Если ни одно из значений нечитаемо или хэш повреждён — форматируем.
 */
static int bnkfs_load_or_format(struct super_block *sb, struct bnkfs_sb_info *sbi)
{
	struct bnkfs_disk_sb dsb;
	int err1, err2;

	err1 = bnkfs_read_sb_copy(sb, sbi->sb1_offset, &dsb);
	if (err1 == 0)
		goto loaded;

	pr_warn("bnkfs: основной суперблок повреждён (err=%d), пробую копию\n", err1);
	err2 = bnkfs_read_sb_copy(sb, sbi->sb2_offset, &dsb);
	if (err2 == 0) {
		/* Восстановим основной из копии. */
		pr_info("bnkfs: восстанавливаю основной суперблок из копии\n");
		bnkfs_write_sb_copy(sb, sbi->sb1_offset, &dsb);
		goto loaded;
	}

	pr_warn("bnkfs: обе копии суперблока невалидны (err1=%d err2=%d), форматирую\n",
		err1, err2);
	return bnkfs_format_device(sb, sbi);

loaded:
	/* Сверяем параметры in-memory с сохранёнными. Если расходятся — это
	 * ФС с другими параметрами, форматируем заново под новые. */
	if (dsb.file_count    != sbi->file_count   ||
	    dsb.file_sectors  != sbi->file_sectors ||
	    dsb.sb2_offset    != sbi->sb2_offset) {
		pr_info("bnkfs: параметры ФС изменились, переформатирую\n");
		return bnkfs_format_device(sb, sbi);
	}
	return 0;
}

/* ------------------------------------------------------------- */
/*                  Чтение/запись содержимого файлов               */
/* ------------------------------------------------------------- */

/* Универсальный путь read: копируем из секторов в userspace через sb_bread. */
static ssize_t bnkfs_file_read(struct file *file, char __user *buf,
			       size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
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
		unsigned int to_copy = min_t(size_t, BNKFS_SECTOR_SIZE - off, len - copied);
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

/* Запись: тоже посекторно, через sb_bread + mark_buffer_dirty. */
static ssize_t bnkfs_file_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	unsigned int idx = (unsigned int)(inode->i_ino - BNKFS_FILE_INO_BASE);
	loff_t pos = *ppos;
	loff_t size = inode->i_size;
	size_t written = 0;
	sector_t base;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return -ENOSPC; /* файлы фиксированного размера, расти не могут */
	if (pos + len > size)
		len = size - pos;

	base = bnkfs_file_start_sector(sbi, idx);

	while (written < len) {
		u64 cur = (u64)pos + written;
		sector_t sec = base + (sector_t)(cur / BNKFS_SECTOR_SIZE);
		unsigned int off = (unsigned int)(cur % BNKFS_SECTOR_SIZE);
		unsigned int to_copy = min_t(size_t, BNKFS_SECTOR_SIZE - off, len - written);
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

/* Принудительный сброс грязных буферов на устройство. */
static int bnkfs_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct super_block *sb = file_inode(file)->i_sb;
	sync_blockdev(sb->s_bdev);
	return 0;
}

/* ------------------------------------------------------------- */
/*                            IOCTL                                */
/* ------------------------------------------------------------- */

/* Обнулить все сектора всех файлов. */
static long bnkfs_ioc_zero_all(struct super_block *sb)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	unsigned int i;
	unsigned int total = sbi->file_count * sbi->file_sectors;

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

/* Стереть ФС — затереть оба суперблока (магическое поле и хэш). */
static long bnkfs_ioc_erase_fs(struct super_block *sb)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
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

/* Посчитать CRC32 по всему содержимому файла. */
static int bnkfs_file_hash(struct super_block *sb, unsigned int idx, __u32 *out_hash)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
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

/* Вернуть метаинформацию обо всех файлах. */
static long bnkfs_ioc_get_hashes(struct super_block *sb, void __user *argp)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	struct bnkfs_hashes_req req;
	struct bnkfs_file_meta *entries;
	unsigned int i, to_write;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	to_write = min_t(unsigned int, req.max_count, sbi->file_count);
	entries = kzalloc(sizeof(*entries) * to_write, GFP_KERNEL);
	if (!entries && to_write)
		return -ENOMEM;

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

	req.count = sbi->file_count; /* реальное число файлов в ФС */
	if (copy_to_user(argp, &req, sizeof(req)))
		ret = -EFAULT;

out:
	kfree(entries);
	return ret;
}

/* Вернуть маппинг секторов для заданного по имени файла. */
static long bnkfs_ioc_get_mapping(struct super_block *sb, void __user *argp)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	struct bnkfs_mapping req;
	int idx;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.name[BNKFS_NAME_MAX - 1] = '\0';

	idx = bnkfs_parse_name(sbi, req.name, strnlen(req.name, BNKFS_NAME_MAX));
	if (idx < 0)
		return -ENOENT;

	req.start_sector = bnkfs_file_start_sector(sbi, idx);
	req.sector_count = sbi->file_sectors;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

/* Единый диспетчер IOCTL, доступный на любом файле и на корневой директории. */
static long bnkfs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
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

/* ------------------------------------------------------------- */
/*                      Директория и lookup                        */
/* ------------------------------------------------------------- */

/* iterate_shared: эмитим виртуальные записи file_0000 ... file_NNNN. */
static int bnkfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
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

static struct inode *bnkfs_get_file_inode(struct super_block *sb, unsigned int idx);

/* Lookup в корне: парсим имя как file_NNNN и создаём inode. */
static struct dentry *bnkfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	int idx;
	struct inode *inode = NULL;

	if (dentry->d_name.len > sbi->max_name_len)
		return ERR_PTR(-ENAMETOOLONG);

	idx = bnkfs_parse_name(sbi, dentry->d_name.name, dentry->d_name.len);
	if (idx >= 0)
		inode = bnkfs_get_file_inode(sb, (unsigned int)idx);

	return d_splice_alias(inode, dentry);
}

static const struct file_operations bnkfs_dir_fops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = bnkfs_iterate,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = bnkfs_unlocked_ioctl,
};

static const struct inode_operations bnkfs_dir_iops = {
	.lookup = bnkfs_lookup,
};

/* ------------------------------------------------------------- */
/*                     Операции с файлами                          */
/* ------------------------------------------------------------- */

static const struct file_operations bnkfs_file_fops = {
	.owner          = THIS_MODULE,
	.read           = bnkfs_file_read,
	.write          = bnkfs_file_write,
	.llseek         = generic_file_llseek,
	.fsync          = bnkfs_file_fsync,
	.unlocked_ioctl = bnkfs_unlocked_ioctl,
};

static const struct inode_operations bnkfs_file_iops = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

/* Создать (или получить из кэша) inode для файла с индексом idx. */
static struct inode *bnkfs_get_file_inode(struct super_block *sb, unsigned int idx)
{
	struct bnkfs_sb_info *sbi = sb->s_fs_info;
	unsigned long ino = BNKFS_FILE_INO_BASE + idx;
	struct inode *inode = iget_locked(sb, ino);

	if (!inode)
		return NULL;
	if (!(inode->i_state & I_NEW))
		return inode; /* уже инициализирован ранее */

	inode->i_mode = S_IFREG | 0644;
	inode->i_uid  = GLOBAL_ROOT_UID;
	inode->i_gid  = GLOBAL_ROOT_GID;
	inode->i_size = (loff_t)sbi->file_sectors * BNKFS_SECTOR_SIZE;
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_current(inode);
	inode->i_op = &bnkfs_file_iops;
	inode->i_fop = &bnkfs_file_fops;
	unlock_new_inode(inode);
	return inode;
}

/* Создать корневой inode (директория). */
static struct inode *bnkfs_make_root_inode(struct super_block *sb)
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

/* ------------------------------------------------------------- */
/*               Операции суперблока (VFS-уровень)                 */
/* ------------------------------------------------------------- */

static void bnkfs_put_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static int bnkfs_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	struct bnkfs_sb_info *sbi = sb->s_fs_info;

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

/* ------------------------------------------------------------- */
/*                      Mount / fill_super                         */
/* ------------------------------------------------------------- */

static int bnkfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bnkfs_sb_info *sbi;
	struct inode *root;
	sector_t total_sectors;
	__u32 data_start, max_files;
	int err;

	/* Проверка имени устройства, если пользователь указал disk_name. */
	if (disk_name && disk_name[0]) {
		const char *devname = sb->s_bdev ? sb->s_bdev->bd_disk->disk_name : "";
		/* fc->source содержит путь /dev/xxx; bd_disk->disk_name — короткое имя */
		if (devname && !strstr(disk_name, devname) && strcmp(disk_name, devname) != 0) {
			pr_warn("bnkfs: mount на %s, а параметр disk_name=%s — продолжаю всё равно\n",
				devname, disk_name);
		}
	}

	/* Базовая валидация параметров. */
	if (sb1_offset == sb2_offset) {
		pr_err("bnkfs: sb1_offset и sb2_offset должны различаться\n");
		return -EINVAL;
	}
	if (max_file_sectors < 1) {
		pr_err("bnkfs: max_file_sectors должен быть >= 1\n");
		return -EINVAL;
	}
	if (max_name_len < 8 || max_name_len > BNKFS_NAME_MAX - 1) {
		pr_err("bnkfs: max_name_len должен быть в [8 .. %u]\n", BNKFS_NAME_MAX - 1);
		return -EINVAL;
	}

	/* Настраиваем блок-сайз ФС. */
	if (!sb_set_blocksize(sb, BNKFS_BLOCK_SIZE)) {
		pr_err("bnkfs: не удалось установить blocksize=%u\n", BNKFS_BLOCK_SIZE);
		return -EINVAL;
	}

	total_sectors = bdev_nr_sectors(sb->s_bdev);
	if (total_sectors < 16) {
		pr_err("bnkfs: устройство слишком маленькое (%llu секторов)\n",
		       (unsigned long long)total_sectors);
		return -ENOSPC;
	}

	/* Данные начинаются сразу после самой далёкой копии суперблока. */
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

	/* Подсчитываем, сколько цифр нужно для имён файлов: минимум 4. */
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
		(unsigned long long)total_sectors, data_start, max_files, max_file_sectors);
	return 0;
}

/* ------------------------------------------------------------- */
/*                  fs_context API (ядро 6.x)                      */
/* ------------------------------------------------------------- */

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
	/* Сбрасываем буферы перед отмонтированием. */
	if (sb->s_bdev)
		sync_blockdev(sb->s_bdev);
	kill_block_super(sb);
}

static struct file_system_type bnkfs_type = {
	.owner            = THIS_MODULE,
	.name             = "bnkfs",
	.init_fs_context  = bnkfs_init_fs_context,
	.kill_sb          = bnkfs_kill_sb,
	.fs_flags         = FS_REQUIRES_DEV,
};

/* ------------------------------------------------------------- */
/*                     init / exit модуля                          */
/* ------------------------------------------------------------- */

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
