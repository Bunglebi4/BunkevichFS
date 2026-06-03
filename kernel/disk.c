#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <linux/statfs.h>

#include "internal.h"

__u32 bfs_sb_checksum(const struct bfs_disk_sb *dsb)
{
	size_t len = offsetof(struct bfs_disk_sb, hash);
	return crc32(0, (const u8 *)dsb, len);
}

static int bfs_sb_flush(struct super_block *sb, sector_t where,
			const struct bfs_disk_sb *dsb)
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

static int bfs_sb_fetch(struct super_block *sb, sector_t where,
			struct bfs_disk_sb *out)
{
	struct buffer_head *bh = sb_bread(sb, where);
	__u32 expected;

	if (!bh)
		return -EIO;
	memcpy(out, bh->b_data, sizeof(*out));
	brelse(bh);

	if (out->magic != BFS_SIG)
		return -EINVAL;
	expected = bfs_sb_checksum(out);
	if (expected != out->hash)
		return -EILSEQ;
	return 0;
}

static int bfs_wipe_files(struct super_block *sb, struct bfs_state *st)
{
	unsigned int i;
	unsigned int total = st->file_count * st->file_span;

	for (i = 0; i < total; i++) {
		struct buffer_head *bh = sb_bread(sb, st->data_origin + i);

		if (!bh)
			return -EIO;
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
	}
	return 0;
}

static int bfs_lay_out(struct super_block *sb, struct bfs_state *st)
{
	struct bfs_disk_sb dsb;
	int rc;

	pr_info("bnkfs: форматирую устройство (file_count=%u, file_sectors=%u)\n",
		st->file_count, st->file_span);

	memset(&dsb, 0, sizeof(dsb));
	dsb.magic         = BFS_SIG;
	dsb.version       = 1;
	dsb.total_sectors = st->total_sectors;
	dsb.sb_a          = st->sb_a;
	dsb.sb_b          = st->sb_b;
	dsb.data_origin   = st->data_origin;
	dsb.file_count    = st->file_count;
	dsb.file_span     = st->file_span;
	dsb.max_name      = st->max_name;
	dsb.name_digits   = st->name_digits;
	dsb.hash          = bfs_sb_checksum(&dsb);

	rc = bfs_sb_flush(sb, st->sb_a, &dsb);
	if (rc)
		return rc;
	rc = bfs_sb_flush(sb, st->sb_b, &dsb);
	if (rc)
		return rc;
	rc = bfs_wipe_files(sb, st);
	if (rc)
		return rc;
	sync_blockdev(sb->s_bdev);
	return 0;
}

int bfs_mount_or_format(struct super_block *sb, struct bfs_state *st)
{
	struct bfs_disk_sb dsb;
	int a, b;
	bool from_backup = false;

	a = bfs_sb_fetch(sb, st->sb_a, &dsb);
	if (a != 0) {
		pr_warn("bnkfs: основной суперблок невалиден (err=%d), пробую копию\n", a);
		b = bfs_sb_fetch(sb, st->sb_b, &dsb);
		if (b != 0) {
			if (a == -EINVAL && b == -EINVAL) {
				pr_info("bnkfs: чистое устройство, форматирую\n");
				return bfs_lay_out(sb, st);
			}
			pr_err("bnkfs: суперблок повреждён (err1=%d err2=%d), монтирование отменено\n",
			       a, b);
			return -EILSEQ;
		}
		from_backup = true;
	}

	if (dsb.total_sectors > st->total_sectors) {
		pr_err("bnkfs: размер устройства меньше, чем записано в SB (%u < %u)\n",
		       st->total_sectors, dsb.total_sectors);
		return -EINVAL;
	}

	st->sb_a        = dsb.sb_a;
	st->sb_b        = dsb.sb_b;
	st->data_origin = dsb.data_origin;
	st->file_count  = dsb.file_count;
	st->file_span   = dsb.file_span;
	st->max_name    = dsb.max_name;
	st->name_digits = dsb.name_digits;

	if (from_backup) {
		pr_info("bnkfs: восстанавливаю основной суперблок из копии\n");
		bfs_sb_flush(sb, st->sb_a, &dsb);
	}
	return 0;
}

static void bfs_release(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static int bfs_statvfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	struct bfs_state *st = bfs_state(sb);

	buf->f_type    = BFS_SIG;
	buf->f_bsize   = sb->s_blocksize;
	buf->f_blocks  = st->total_sectors;
	buf->f_bfree   = 0;
	buf->f_bavail  = 0;
	buf->f_files   = st->file_count;
	buf->f_ffree   = 0;
	buf->f_namelen = st->max_name;
	return 0;
}

static const struct super_operations bfs_super_ops = {
	.statfs    = bfs_statvfs,
	.put_super = bfs_release,
};

static unsigned int bfs_count_digits(unsigned int n)
{
	unsigned int d = 1;

	while (n >= 10) { d++; n /= 10; }
	return d;
}

#define BFS_NAME_PREFIX_LEN 5  /* strlen("file_") */

static unsigned int bfs_max_files_for_name(unsigned int max_name)
{
	unsigned int avail = max_name - BFS_NAME_PREFIX_LEN;
	unsigned int cap = 1;
	unsigned int i;

	for (i = 0; i < avail && cap <= 1000000000u / 10; i++)
		cap *= 10;
	return cap;
}

int bfs_fill_root_super(struct super_block *sb, struct fs_context *fc)
{
	struct bfs_state *st;
	struct inode *root;
	sector_t total_sectors;
	__u32 origin, files;
	unsigned int p_sb1 = bfs_param_sb1();
	unsigned int p_sb2 = bfs_param_sb2();
	unsigned int p_max = bfs_param_maxname();
	unsigned int p_span = bfs_param_filespan();
	const char *p_disk = bfs_param_diskname();
	int rc;

	if (p_sb1 == p_sb2) {
		pr_err("bnkfs: sb1_offset и sb2_offset должны различаться\n");
		return -EINVAL;
	}
	if (p_span < 1) {
		pr_err("bnkfs: max_file_sectors должен быть >= 1\n");
		return -EINVAL;
	}
	if (p_max < BFS_NAME_PREFIX_LEN + 1 || p_max > BFS_MAX_NAME - 1) {
		pr_err("bnkfs: max_name_len должен быть в [%u .. %u]\n",
		       BFS_NAME_PREFIX_LEN + 1, BFS_MAX_NAME - 1);
		return -EINVAL;
	}
	if (!sb_set_blocksize(sb, BFS_BLOCK)) {
		pr_err("bnkfs: не удалось установить blocksize=%u\n", BFS_BLOCK);
		return -EINVAL;
	}

	total_sectors = bdev_nr_sectors(sb->s_bdev);
	if (total_sectors < 16) {
		pr_err("bnkfs: устройство слишком маленькое (%llu секторов)\n",
		       (unsigned long long)total_sectors);
		return -ENOSPC;
	}

	origin = max(p_sb1, p_sb2) + 1;
	if (origin >= total_sectors) {
		pr_err("bnkfs: смещения суперблоков выходят за пределы устройства\n");
		return -EINVAL;
	}
	files = (total_sectors - origin) / p_span;
	if (!files) {
		pr_err("bnkfs: на устройстве не помещается ни одного файла\n");
		return -ENOSPC;
	}
	{
		unsigned int name_cap = bfs_max_files_for_name(p_max);

		if (files > name_cap) {
			pr_info("bnkfs: количество файлов ограничено %u (max_name_len=%u)\n",
				name_cap, p_max);
			files = name_cap;
		}
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->sb_a          = p_sb1;
	st->sb_b          = p_sb2;
	st->data_origin   = origin;
	st->file_count    = files;
	st->file_span     = p_span;
	st->max_name      = p_max;
	st->total_sectors = total_sectors;
	st->name_digits   = bfs_count_digits(files - 1);

	sb->s_magic     = BFS_SIG;
	sb->s_op        = &bfs_super_ops;
	sb->s_fs_info   = st;
	sb->s_maxbytes  = (loff_t)st->file_span * BFS_SECTOR;
	sb->s_time_gran = 1;

	rc = bfs_mount_or_format(sb, st);
	if (rc) {
		kfree(st);
		sb->s_fs_info = NULL;
		return rc;
	}

	root = bfs_spawn_root(sb);
	if (!root) {
		kfree(st);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		kfree(st);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}

	pr_info("bnkfs: смонтировано: total=%llu, data_start=%u, files=%u x %u секторов\n",
		(unsigned long long)total_sectors, origin, files, p_span);

	if (p_disk && p_disk[0]) {
		const char *dn = sb->s_bdev ? sb->s_bdev->bd_disk->disk_name : "";

		if (dn && strcmp(p_disk, dn) != 0 && !strstr(p_disk, dn))
			pr_warn("bnkfs: mount на %s, а параметр disk_name=%s\n", dn, p_disk);
	}
	return 0;
}
