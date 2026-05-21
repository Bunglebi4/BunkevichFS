#ifndef BNKFS_IOCTL_H
#define BNKFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define BNKFS_IOCTL_NAME_MAX 64

struct bnkfs_ioctl_hash_entry {
	char name[BNKFS_IOCTL_NAME_MAX];
	__u64 start_sector;
	__u32 sectors;
	__u32 hash;
};

struct bnkfs_ioctl_hash_list {
	__u64 entries_ptr;
	__u32 capacity;
	__u32 count;
	__u32 total_count;
};

struct bnkfs_ioctl_sector_map_req {
	char name[BNKFS_IOCTL_NAME_MAX];
	__u64 start_sector;
	__u32 sectors;
};

#define BNKFS_IOC_MAGIC 'k'

#define BNKFS_IOC_ZERO_ALL _IO(BNKFS_IOC_MAGIC, 1)
#define BNKFS_IOC_ERASE_FS _IO(BNKFS_IOC_MAGIC, 2)
#define BNKFS_IOC_GET_HASHES _IOWR(BNKFS_IOC_MAGIC, 3, struct bnkfs_ioctl_hash_list)
#define BNKFS_IOC_GET_MAP _IOWR(BNKFS_IOC_MAGIC, 4, struct bnkfs_ioctl_sector_map_req)

#endif
