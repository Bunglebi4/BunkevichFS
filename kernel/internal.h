#ifndef BFS_INTERNAL_H
#define BFS_INTERNAL_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/blkdev.h>

#include "bnkfs.h"

#define BFS_BLOCK     BFS_SECTOR
#define BFS_INO_ROOT  1u
#define BFS_INO_FIRST 2u

struct bfs_disk_sb {
	__u32 magic;
	__u32 version;
	__u32 total_sectors;
	__u32 sb_a;
	__u32 sb_b;
	__u32 data_origin;
	__u32 file_count;
	__u32 file_span;
	__u32 max_name;
	__u32 name_digits;
	__u32 reserved[21];
	__u32 hash;
} __packed;

struct bfs_state {
	__u32 sb_a;
	__u32 sb_b;
	__u32 data_origin;
	__u32 file_count;
	__u32 file_span;
	__u32 max_name;
	__u32 name_digits;
	__u32 total_sectors;
};

static inline struct bfs_state *bfs_state(struct super_block *sb)
{
	return (struct bfs_state *)sb->s_fs_info;
}

static inline sector_t bfs_file_origin(struct bfs_state *st, unsigned int idx)
{
	return (sector_t)st->data_origin + (sector_t)idx * st->file_span;
}

__u32  bfs_sb_checksum(const struct bfs_disk_sb *dsb);
int    bfs_mount_or_format(struct super_block *sb, struct bfs_state *st);
int    bfs_fill_root_super(struct super_block *sb, struct fs_context *fc);

void   bfs_render_name(struct bfs_state *st, unsigned int idx,
		       char *out, size_t outsz);
int    bfs_parse_name(struct bfs_state *st, const char *name, size_t len);

struct inode *bfs_spawn_file(struct super_block *sb, unsigned int idx);
struct inode *bfs_spawn_root(struct super_block *sb);

extern const struct file_operations  bfs_file_fops;
extern const struct inode_operations bfs_file_iops;
extern const struct file_operations  bfs_dir_fops;
extern const struct inode_operations bfs_dir_iops;

long bfs_dispatch_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

unsigned int bfs_param_sb1(void);
unsigned int bfs_param_sb2(void);
unsigned int bfs_param_maxname(void);
unsigned int bfs_param_filespan(void);
const char  *bfs_param_diskname(void);

#endif
