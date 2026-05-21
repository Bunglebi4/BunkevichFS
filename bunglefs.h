/*
 * BunkevichFS / SimpleFS — shared definitions for kernel module
 * and userspace CLI (ioctl interface, on-disk magic, layout limits).
 */
#ifndef BUNGLEFS_H
#define BUNGLEFS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef int32_t  __s32;
typedef uint32_t __le32;
#endif

#define BUNGLEFS_MAGIC          0x42554E47  /* "BUNG" */
#define BUNGLEFS_SECTOR_SIZE    512
#define BUNGLEFS_MAX_FILES      64
#define BUNGLEFS_NAME_LEN       32

/* On-disk file entry inside the superblock sector */
struct bunglefs_file_entry {
	char    name[BUNGLEFS_NAME_LEN];
	__le32  start_sector;
	__le32  size_sectors;
	__le32  content_hash;     /* CRC32 of file content; 0 if empty */
	__le32  reserved;
};

/* Superblock fits in one sector (512 bytes) — keep field count modest. */
struct bunglefs_dsb {
	__le32  magic;
	__le32  file_count;
	__le32  file_size_sectors;     /* M */
	__le32  max_name_len;
	__le32  sb1_offset;            /* sector index of primary SB    */
	__le32  sb2_offset;            /* sector index of backup SB     */
	__le32  first_file_sector;     /* where file area begins        */
	__le32  total_sectors;
	__le32  hash;                  /* CRC32 over everything except this field */
	__le32  pad[7];
	struct bunglefs_file_entry files[BUNGLEFS_MAX_FILES];
} __attribute__((packed));

/* IOCTL interface */
#define BUNGLEFS_IOC_MAGIC      'B'

struct bunglefs_meta_entry {
	char     name[BUNGLEFS_NAME_LEN];
	__u32    start_sector;
	__u32    size_sectors;
	__u32    content_hash;
};

struct bunglefs_meta_list {
	__u32    count;
	struct bunglefs_meta_entry entries[BUNGLEFS_MAX_FILES];
};

struct bunglefs_file_map {
	char     name[BUNGLEFS_NAME_LEN]; /* in  */
	__u32    start_sector;            /* out */
	__u32    size_sectors;            /* out */
	__u32    found;                   /* out: 0/1 */
};

#define BUNGLEFS_IOC_ZERO_ALL   _IO(BUNGLEFS_IOC_MAGIC, 1)
#define BUNGLEFS_IOC_WIPE_FS    _IO(BUNGLEFS_IOC_MAGIC, 2)
#define BUNGLEFS_IOC_LIST_META  _IOR(BUNGLEFS_IOC_MAGIC, 3, struct bunglefs_meta_list)
#define BUNGLEFS_IOC_FILE_MAP   _IOWR(BUNGLEFS_IOC_MAGIC, 4, struct bunglefs_file_map)

#endif /* BUNGLEFS_H */
