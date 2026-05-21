/*
 * bnkfs_internal.h — внутренний заголовок BunkevichFS, недоступный userspace.
 * Содержит дисковые структуры, in-memory состояние и прототипы функций,
 * разделённых между super.c / inode.c / file.c.
 */
#ifndef BNKFS_INTERNAL_H
#define BNKFS_INTERNAL_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/blkdev.h>

#include "bnkfs.h"

#define BNKFS_BLOCK_SIZE    BNKFS_SECTOR_SIZE
#define BNKFS_ROOT_INO      1u
#define BNKFS_FILE_INO_BASE 2u

/*
 * Дисковая структура суперблока. Должна целиком влезать в один сектор.
 * Хэш считается по всем полям выше hash (offsetof) — простая защита от
 * случайного повреждения.
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
	__u32 hash;              /* CRC32 по полям выше */
} __packed;

/* In-memory информация о ФС, хранится в sb->s_fs_info. */
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

/* Возвращает sb_info, спрятанный в s_fs_info. */
static inline struct bnkfs_sb_info *BNKFS_SB(struct super_block *sb)
{
	return (struct bnkfs_sb_info *)sb->s_fs_info;
}

/* Стартовый сектор файла с индексом idx. */
static inline sector_t bnkfs_file_start_sector(struct bnkfs_sb_info *sbi,
					       unsigned int idx)
{
	return (sector_t)sbi->data_start_sector +
	       (sector_t)idx * sbi->file_sectors;
}

/* super.c */
__u32 bnkfs_sb_calc_hash(const struct bnkfs_disk_sb *dsb);
int   bnkfs_load_or_format(struct super_block *sb, struct bnkfs_sb_info *sbi);
int   bnkfs_fill_super(struct super_block *sb, struct fs_context *fc);

/* inode.c */
void  bnkfs_make_name(struct bnkfs_sb_info *sbi, unsigned int idx,
		      char *out, size_t outsz);
int   bnkfs_parse_name(struct bnkfs_sb_info *sbi,
		       const char *name, size_t len);
struct inode *bnkfs_get_file_inode(struct super_block *sb, unsigned int idx);
struct inode *bnkfs_make_root_inode(struct super_block *sb);

/* file.c */
extern const struct file_operations  bnkfs_file_fops;
extern const struct inode_operations bnkfs_file_iops;
extern const struct file_operations  bnkfs_dir_fops;
extern const struct inode_operations bnkfs_dir_iops;

long bnkfs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* BNKFS_INTERNAL_H */
