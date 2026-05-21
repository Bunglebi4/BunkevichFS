/*
 * bnkfs_ioctl.h — общие определения IOCTL для ядерного модуля BunkevichFS
 * и userspace-утилиты. Заголовок инклюдится как из кода ядра, так и из C++.
 */
#ifndef BNKFS_IOCTL_H
#define BNKFS_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

/* Магическое число для IOCTL команд нашей ФС. */
#define BNKFS_IOC_MAGIC 'B'

/* Максимальная длина имени файла в структурах IOCTL (с запасом). */
#define BNKFS_NAME_MAX 64

/* Описание одного файла в ответе на BNKFS_IOC_GET_HASHES. */
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

/* Запрос маппинга секторов для заданного по имени файла. */
struct bnkfs_mapping {
	char  name[BNKFS_NAME_MAX]; /* in: имя файла */
	__u32 start_sector;         /* out: первый сектор */
	__u32 sector_count;         /* out: число секторов */
};

/* Обнулить содержимое всех файлов. */
#define BNKFS_IOC_ZERO_ALL    _IO(BNKFS_IOC_MAGIC, 1)
/* Стереть ФС (затереть оба суперблока, чтобы при следующем mount была ошибка). */
#define BNKFS_IOC_ERASE_FS    _IO(BNKFS_IOC_MAGIC, 2)
/* Получить список метаинформации (имена и хэши) всех файлов. */
#define BNKFS_IOC_GET_HASHES  _IOWR(BNKFS_IOC_MAGIC, 3, struct bnkfs_hashes_req)
/* Получить маппинг секторов для одного файла. */
#define BNKFS_IOC_GET_MAPPING _IOWR(BNKFS_IOC_MAGIC, 4, struct bnkfs_mapping)

#endif /* BNKFS_IOCTL_H */
