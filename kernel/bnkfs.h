/*
 * bnkfs.h — общие определения BunkevichFS, доступные как ядру, так и userspace.
 * Включает IOCTL-команды и связанные с ними структуры.
 */
#ifndef BNKFS_H
#define BNKFS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>  /* даёт согласованные с ядром __u32/__u64 */
#include <sys/ioctl.h>
#endif

#define BNKFS_FS_NAME      "bnkfs"
#define BNKFS_MAGIC        0x424E4B46u  /* "BNKF" */
#define BNKFS_SECTOR_SIZE  512u
#define BNKFS_NAME_MAX     64u

/* Метаинформация об одном файле (используется IOCTL GET_HASHES). */
struct bnkfs_file_meta {
	char  name[BNKFS_NAME_MAX]; /* имя файла */
	__u32 hash;                 /* CRC32 содержимого файла */
	__u32 start_sector;         /* первый сектор файла на диске */
	__u32 sector_count;         /* сколько секторов занимает */
	__u32 _pad;
};

/*
 * Запрос на получение метаинформации обо всех файлах.
 * entries указывает на буфер размером max_count элементов в userspace,
 * ядро записывает туда не более max_count записей, в count кладёт фактическое.
 */
struct bnkfs_hashes_req {
	__u32 max_count;
	__u32 count;
	__u64 entries; /* указатель на массив struct bnkfs_file_meta */
};

/* Запрос маппинга секторов для заданного файла. */
struct bnkfs_mapping {
	char  name[BNKFS_NAME_MAX]; /* in: имя файла */
	__u32 start_sector;         /* out: первый сектор */
	__u32 sector_count;         /* out: число секторов */
};

#define BNKFS_IOC_MAGIC 'B'
#define BNKFS_IOC_ZERO_ALL    _IO(BNKFS_IOC_MAGIC, 1)
#define BNKFS_IOC_ERASE_FS    _IO(BNKFS_IOC_MAGIC, 2)
#define BNKFS_IOC_GET_HASHES  _IOWR(BNKFS_IOC_MAGIC, 3, struct bnkfs_hashes_req)
#define BNKFS_IOC_GET_MAPPING _IOWR(BNKFS_IOC_MAGIC, 4, struct bnkfs_mapping)

#endif /* BNKFS_H */
