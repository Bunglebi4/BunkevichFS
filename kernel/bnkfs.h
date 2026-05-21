#ifndef BNKFS_H
#define BNKFS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define BFS_NAME       "bnkfs"
#define BFS_SIG        0x424E4B46u
#define BFS_SECTOR     512u
#define BFS_MAX_NAME   64u

struct bfs_meta {
	char  name[BFS_MAX_NAME];
	__u32 hash;
	__u32 start_sector;
	__u32 sector_count;
	__u32 _pad;
};

struct bfs_hashes {
	__u32 max_count;
	__u32 count;
	__u64 entries;
};

struct bfs_map {
	char  name[BFS_MAX_NAME];
	__u32 start_sector;
	__u32 sector_count;
};

#define BFS_IOC_MAG 'B'
#define BFS_IOC_ZERO    _IO(BFS_IOC_MAG, 1)
#define BFS_IOC_WIPE    _IO(BFS_IOC_MAG, 2)
#define BFS_IOC_HASHES  _IOWR(BFS_IOC_MAG, 3, struct bfs_hashes)
#define BFS_IOC_MAP     _IOWR(BFS_IOC_MAG, 4, struct bfs_map)

#endif
