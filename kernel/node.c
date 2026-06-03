#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/string.h>
#include <linux/dcache.h>

#include "internal.h"

void bfs_render_name(struct bfs_state *st, unsigned int idx,
		     char *out, size_t outsz)
{
	snprintf(out, outsz, "file_%0*u", st->name_digits, idx);
}

int bfs_parse_name(struct bfs_state *st, const char *name, size_t len)
{
	char buf[BFS_MAX_NAME];
	char ref[BFS_MAX_NAME];
	unsigned int idx;

	if (len >= sizeof(buf))
		return -1;
	memcpy(buf, name, len);
	buf[len] = '\0';

	if (sscanf(buf, "file_%u", &idx) != 1)
		return -1;
	if (idx >= st->file_count)
		return -1;
	bfs_render_name(st, idx, ref, sizeof(ref));
	if (strcmp(buf, ref) != 0)
		return -1;
	return (int)idx;
}

static ssize_t bfs_read(struct file *file, char __user *buf,
			size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bfs_state *st = bfs_state(sb);
	unsigned int idx = (unsigned int)(inode->i_ino - BFS_INO_FIRST);
	loff_t pos = *ppos;
	loff_t size = inode->i_size;
	size_t done = 0;
	sector_t base;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return 0;
	if (pos + len > size)
		len = size - pos;

	base = bfs_file_origin(st, idx);

	while (done < len) {
		u64 cur = (u64)pos + done;
		sector_t sec = base + (sector_t)(cur / BFS_SECTOR);
		unsigned int off = (unsigned int)(cur % BFS_SECTOR);
		unsigned int chunk = min_t(size_t, BFS_SECTOR - off, len - done);
		struct buffer_head *bh = sb_bread(sb, sec);

		if (!bh)
			return -EIO;
		if (copy_to_user(buf + done, bh->b_data + off, chunk)) {
			brelse(bh);
			return -EFAULT;
		}
		brelse(bh);
		done += chunk;
	}
	*ppos = pos + done;
	return done;
}

static ssize_t bfs_write(struct file *file, const char __user *buf,
			 size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct bfs_state *st = bfs_state(sb);
	unsigned int idx = (unsigned int)(inode->i_ino - BFS_INO_FIRST);
	loff_t pos = *ppos;
	loff_t size = inode->i_size;
	size_t done = 0;
	sector_t base;

	if (pos < 0)
		return -EINVAL;
	if (pos >= size)
		return -ENOSPC;
	if (pos + len > size)
		len = size - pos;

	base = bfs_file_origin(st, idx);

	while (done < len) {
		u64 cur = (u64)pos + done;
		sector_t sec = base + (sector_t)(cur / BFS_SECTOR);
		unsigned int off = (unsigned int)(cur % BFS_SECTOR);
		unsigned int chunk = min_t(size_t, BFS_SECTOR - off, len - done);
		struct buffer_head *bh = sb_bread(sb, sec);

		if (!bh)
			return -EIO;
		lock_buffer(bh);
		if (copy_from_user(bh->b_data + off, buf + done, chunk)) {
			unlock_buffer(bh);
			brelse(bh);
			return -EFAULT;
		}
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
		done += chunk;
	}
	*ppos = pos + done;
	return done;
}

static int bfs_sync(struct file *file, loff_t a, loff_t b, int datasync)
{
	sync_blockdev(file_inode(file)->i_sb->s_bdev);
	return 0;
}

static int bfs_crc_file(struct super_block *sb, unsigned int idx, __u32 *out)
{
	struct bfs_state *st = bfs_state(sb);
	sector_t base = bfs_file_origin(st, idx);
	unsigned int i;
	__u32 h = 0;

	for (i = 0; i < st->file_span; i++) {
		struct buffer_head *bh = sb_bread(sb, base + i);

		if (!bh)
			return -EIO;
		h = crc32(h, bh->b_data, BFS_SECTOR);
		brelse(bh);
	}
	*out = h;
	return 0;
}

static long bfs_op_zero(struct super_block *sb)
{
	struct bfs_state *st = bfs_state(sb);
	unsigned int total = st->file_count * st->file_span;
	unsigned int i;

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
	sync_blockdev(sb->s_bdev);
	return 0;
}

static long bfs_op_wipe(struct super_block *sb)
{
	struct bfs_state *st = bfs_state(sb);
	sector_t pair[2] = { st->sb_a, st->sb_b };
	int i;

	for (i = 0; i < 2; i++) {
		struct buffer_head *bh = sb_bread(sb, pair[i]);

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

static long bfs_op_hashes(struct super_block *sb, void __user *argp)
{
	struct bfs_state *st = bfs_state(sb);
	struct bfs_hashes req;
	struct bfs_meta row;
	char __user *dst;
	unsigned int i, n;
	long rc;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	n = min_t(unsigned int, req.max_count, st->file_count);
	dst = (char __user *)(uintptr_t)req.entries;

	for (i = 0; i < n; i++) {
		memset(&row, 0, sizeof(row));
		bfs_render_name(st, i, row.name, sizeof(row.name));
		row.start_sector = bfs_file_origin(st, i);
		row.sector_count = st->file_span;
		rc = bfs_crc_file(sb, i, &row.hash);
		if (rc)
			return rc;
		if (copy_to_user(dst + (size_t)i * sizeof(row),
				 &row, sizeof(row)))
			return -EFAULT;
	}

	req.count = st->file_count;
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long bfs_op_map(struct super_block *sb, void __user *argp)
{
	struct bfs_state *st = bfs_state(sb);
	struct bfs_map req;
	int idx;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.name[BFS_MAX_NAME - 1] = '\0';

	idx = bfs_parse_name(st, req.name, strnlen(req.name, BFS_MAX_NAME));
	if (idx < 0)
		return -ENOENT;

	req.start_sector = bfs_file_origin(st, idx);
	req.sector_count = st->file_span;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

long bfs_dispatch_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case BFS_IOC_ZERO:    return bfs_op_zero(sb);
	case BFS_IOC_WIPE:    return bfs_op_wipe(sb);
	case BFS_IOC_HASHES:  return bfs_op_hashes(sb, argp);
	case BFS_IOC_MAP:     return bfs_op_map(sb, argp);
	default:              return -ENOTTY;
	}
}

const struct file_operations bfs_file_fops = {
	.owner          = THIS_MODULE,
	.read           = bfs_read,
	.write          = bfs_write,
	.llseek         = generic_file_llseek,
	.fsync          = bfs_sync,
	.unlocked_ioctl = bfs_dispatch_ioctl,
};

static int bfs_set_attr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);

	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size != inode->i_size)
			attr->ia_valid &= ~ATTR_SIZE;
	}
	if (attr->ia_valid == 0)
		return 0;
	return simple_setattr(idmap, dentry, attr);
}

const struct inode_operations bfs_file_iops = {
	.setattr = bfs_set_attr,
	.getattr = simple_getattr,
};

static int bfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct bfs_state *st = bfs_state(sb);
	char name[BFS_MAX_NAME];

	if (!dir_emit_dots(file, ctx))
		return 0;

	while (ctx->pos >= 2 && (ctx->pos - 2) < st->file_count) {
		unsigned int idx = (unsigned int)(ctx->pos - 2);

		bfs_render_name(st, idx, name, sizeof(name));
		if (!dir_emit(ctx, name, strlen(name),
			      BFS_INO_FIRST + idx, DT_REG))
			return 0;
		ctx->pos++;
	}
	return 0;
}

static struct dentry *bfs_lookup(struct inode *dir, struct dentry *dentry,
				 unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct bfs_state *st = bfs_state(sb);
	struct inode *inode = NULL;
	int idx;

	if (dentry->d_name.len > st->max_name)
		return ERR_PTR(-ENAMETOOLONG);

	idx = bfs_parse_name(st, dentry->d_name.name, dentry->d_name.len);
	if (idx >= 0)
		inode = bfs_spawn_file(sb, (unsigned int)idx);

	return d_splice_alias(inode, dentry);
}

const struct file_operations bfs_dir_fops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = bfs_readdir,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = bfs_dispatch_ioctl,
};

const struct inode_operations bfs_dir_iops = {
	.lookup = bfs_lookup,
};

struct inode *bfs_spawn_file(struct super_block *sb, unsigned int idx)
{
	struct bfs_state *st = bfs_state(sb);
	unsigned long ino = BFS_INO_FIRST + idx;
	struct inode *inode = iget_locked(sb, ino);

	if (!inode)
		return NULL;
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_mode = S_IFREG | 0644;
	inode->i_uid  = GLOBAL_ROOT_UID;
	inode->i_gid  = GLOBAL_ROOT_GID;
	inode->i_size = (loff_t)st->file_span * BFS_SECTOR;
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_current(inode);
	inode->i_op  = &bfs_file_iops;
	inode->i_fop = &bfs_file_fops;
	unlock_new_inode(inode);
	return inode;
}

struct inode *bfs_spawn_root(struct super_block *sb)
{
	struct inode *inode = iget_locked(sb, BFS_INO_ROOT);

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
	inode->i_op  = &bfs_dir_iops;
	inode->i_fop = &bfs_dir_fops;
	unlock_new_inode(inode);
	return inode;
}
