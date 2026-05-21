#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "bnkfs_ioctl.h"

#define BNKFS_NAME "bnkfs"
#define BNKFS_MAGIC 0x424e4b46U
#define BNKFS_VERSION 1
#define BNKFS_SECTOR_SIZE 512U
#define BNKFS_SB(sb) ((struct bnkfs_sb_info *)((sb)->s_fs_info))

struct bnkfs_disk_super {
	__le32 magic;
	__le32 version;
	__le64 primary_sector;
	__le64 backup_sector;
	__le64 total_sectors;
	__le64 data_start_sector;
	__le32 file_count;
	__le32 file_sectors;
	__le32 max_name_len;
	__le32 checksum;
} __packed;

struct bnkfs_file_entry {
	u32 index;
	u64 start_sector;
	u32 sectors;
	char name[BNKFS_IOCTL_NAME_MAX];
	u32 hash;
};

struct bnkfs_sb_info {
	struct super_block *sb;
	struct bnkfs_disk_super dsb;
	struct bnkfs_file_entry *files;
	u32 file_count;
	u32 file_sectors;
	u32 max_name_len;
	u64 primary_sector;
	u64 backup_sector;
	struct mutex io_lock;
};

static char *disk_name = "";
module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Base block device name or /dev path");

static ulong sb_primary_sector;
module_param(sb_primary_sector, ulong, 0444);
MODULE_PARM_DESC(sb_primary_sector, "Primary superblock sector offset");

static ulong sb_backup_sector = 8;
module_param(sb_backup_sector, ulong, 0444);
MODULE_PARM_DESC(sb_backup_sector, "Backup superblock sector offset");

static uint max_filename_len = 32;
module_param(max_filename_len, uint, 0444);
MODULE_PARM_DESC(max_filename_len, "Maximum file name length");

static uint max_file_sectors = 1;
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "File size in sectors (1..M)");

static DEFINE_MUTEX(bnkfs_ctl_lock);
static struct bnkfs_sb_info *bnkfs_active_sbi;

static u32 bnkfs_super_checksum(const struct bnkfs_disk_super *src)
{
	struct bnkfs_disk_super tmp = *src;

	tmp.checksum = 0;
	return crc32(0, (const u8 *)&tmp, sizeof(tmp));
}

static bool bnkfs_super_valid(const struct bnkfs_disk_super *dsb)
{
	u32 stored = le32_to_cpu(dsb->checksum);

	if (le32_to_cpu(dsb->magic) != BNKFS_MAGIC)
		return false;
	if (le32_to_cpu(dsb->version) != BNKFS_VERSION)
		return false;
	return stored == bnkfs_super_checksum(dsb);
}

static int bnkfs_read_raw_sector(struct super_block *sb, u64 sector, u8 *out)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;
	memcpy(out, bh->b_data, BNKFS_SECTOR_SIZE);
	brelse(bh);
	return 0;
}

static int bnkfs_write_raw_sector(struct super_block *sb, u64 sector, const u8 *in)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;
	memcpy(bh->b_data, in, BNKFS_SECTOR_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	if (buffer_write_io_error(bh)) {
		brelse(bh);
		return -EIO;
	}
	brelse(bh);
	return 0;
}

static int bnkfs_read_disk_super(struct super_block *sb, u64 sector,
				 struct bnkfs_disk_super *dsb)
{
	u8 tmp[BNKFS_SECTOR_SIZE];
	int ret;

	ret = bnkfs_read_raw_sector(sb, sector, tmp);
	if (ret)
		return ret;
	memcpy(dsb, tmp, sizeof(*dsb));
	return 0;
}

static int bnkfs_write_disk_super(struct super_block *sb,
				  const struct bnkfs_sb_info *sbi)
{
	u8 raw[BNKFS_SECTOR_SIZE];
	struct bnkfs_disk_super dsb;
	int ret;

	memset(raw, 0, sizeof(raw));
	dsb = sbi->dsb;
	dsb.checksum = cpu_to_le32(bnkfs_super_checksum(&dsb));
	memcpy(raw, &dsb, sizeof(dsb));

	ret = bnkfs_write_raw_sector(sb, sbi->primary_sector, raw);
	if (ret)
		return ret;
	return bnkfs_write_raw_sector(sb, sbi->backup_sector, raw);
}

static int bnkfs_recompute_hash(struct super_block *sb, struct bnkfs_file_entry *fe)
{
	u32 crc = 0;
	u32 i;
	int ret;
	u8 tmp[BNKFS_SECTOR_SIZE];

	for (i = 0; i < fe->sectors; i++) {
		ret = bnkfs_read_raw_sector(sb, fe->start_sector + i, tmp);
		if (ret)
			return ret;
		crc = crc32(crc, tmp, BNKFS_SECTOR_SIZE);
	}
	fe->hash = crc;
	return 0;
}

static int bnkfs_zero_file(struct super_block *sb, struct bnkfs_file_entry *fe)
{
	u8 zeros[BNKFS_SECTOR_SIZE];
	u32 i;
	int ret;

	memset(zeros, 0, sizeof(zeros));
	for (i = 0; i < fe->sectors; i++) {
		ret = bnkfs_write_raw_sector(sb, fe->start_sector + i, zeros);
		if (ret)
			return ret;
	}
	return bnkfs_recompute_hash(sb, fe);
}

static struct bnkfs_file_entry *bnkfs_find_file(struct bnkfs_sb_info *sbi,
						const char *name, size_t len)
{
	u32 i;

	for (i = 0; i < sbi->file_count; i++) {
		if (strlen(sbi->files[i].name) == len &&
		    !strncmp(sbi->files[i].name, name, len))
			return &sbi->files[i];
	}
	return NULL;
}

static int bnkfs_fill_entries(struct super_block *sb, struct bnkfs_sb_info *sbi,
			      bool zero_data)
{
	u32 i;
	int ret;
	u32 width;
	u64 data_start = le64_to_cpu(sbi->dsb.data_start_sector);

	width = max(1U, min_t(u32, 9, sbi->max_name_len - 4));

	for (i = 0; i < sbi->file_count; i++) {
		struct bnkfs_file_entry *fe = &sbi->files[i];

		fe->index = i;
		fe->start_sector = data_start + (u64)i * sbi->file_sectors;
		fe->sectors = sbi->file_sectors;
		snprintf(fe->name, sizeof(fe->name), "f%0*u", width, i);

		if (zero_data)
			ret = bnkfs_zero_file(sb, fe);
		else
			ret = bnkfs_recompute_hash(sb, fe);
		if (ret)
			return ret;
	}
	return 0;
}

static ssize_t bnkfs_file_read(struct file *filp, char __user *buf,
			       size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_file_entry *fe = inode->i_private;
	loff_t size = i_size_read(inode);
	size_t done = 0;
	int ret = 0;

	if (!fe)
		return -EIO;
	if (*ppos >= size)
		return 0;
	if (len > size - *ppos)
		len = size - *ppos;

	mutex_lock(&BNKFS_SB(sb)->io_lock);
	while (done < len) {
		u64 file_pos = *ppos + done;
		u32 sec_idx = div_u64(file_pos, BNKFS_SECTOR_SIZE);
		u32 off = file_pos % BNKFS_SECTOR_SIZE;
		size_t chunk = min_t(size_t, BNKFS_SECTOR_SIZE - off, len - done);
		struct buffer_head *bh;

		bh = sb_bread(sb, fe->start_sector + sec_idx);
		if (!bh) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(buf + done, bh->b_data + off, chunk)) {
			brelse(bh);
			ret = -EFAULT;
			break;
		}
		brelse(bh);
		done += chunk;
	}
	mutex_unlock(&BNKFS_SB(sb)->io_lock);

	if (ret && !done)
		return ret;
	*ppos += done;
	return done;
}

static ssize_t bnkfs_file_write(struct file *filp, const char __user *buf,
				size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct bnkfs_file_entry *fe = inode->i_private;
	loff_t size = i_size_read(inode);
	size_t done = 0;
	int ret = 0;

	if (!fe)
		return -EIO;
	if (*ppos >= size)
		return -ENOSPC;
	if (len > size - *ppos)
		len = size - *ppos;

	mutex_lock(&BNKFS_SB(sb)->io_lock);
	while (done < len) {
		u64 file_pos = *ppos + done;
		u32 sec_idx = div_u64(file_pos, BNKFS_SECTOR_SIZE);
		u32 off = file_pos % BNKFS_SECTOR_SIZE;
		size_t chunk = min_t(size_t, BNKFS_SECTOR_SIZE - off, len - done);
		struct buffer_head *bh;

		bh = sb_bread(sb, fe->start_sector + sec_idx);
		if (!bh) {
			ret = -EIO;
			break;
		}
		if (copy_from_user(bh->b_data + off, buf + done, chunk)) {
			brelse(bh);
			ret = -EFAULT;
			break;
		}
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		if (buffer_write_io_error(bh)) {
			brelse(bh);
			ret = -EIO;
			break;
		}
		brelse(bh);
		done += chunk;
	}
	if (!ret)
		ret = bnkfs_recompute_hash(sb, fe);
	mutex_unlock(&BNKFS_SB(sb)->io_lock);

	if (ret && !done)
		return ret;
	*ppos += done;
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
	return done;
}

static const struct file_operations bnkfs_file_ops = {
	.owner = THIS_MODULE,
	.read = bnkfs_file_read,
	.write = bnkfs_file_write,
	.llseek = generic_file_llseek,
};

static int bnkfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct bnkfs_sb_info *sbi = BNKFS_SB(inode->i_sb);
	u32 i;

	if (!dir_emit_dots(file, ctx))
		return 0;

	i = ctx->pos - 2;
	while (i < sbi->file_count) {
		struct bnkfs_file_entry *fe = &sbi->files[i];

		if (!dir_emit(ctx, fe->name, strlen(fe->name), fe->index + 10, DT_REG))
			return 0;
		ctx->pos++;
		i++;
	}
	return 0;
}

static struct dentry *bnkfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(dir->i_sb);
	struct bnkfs_file_entry *fe;
	struct inode *inode = NULL;

	(void)flags;

	fe = bnkfs_find_file(sbi, dentry->d_name.name, dentry->d_name.len);
	if (fe) {
		inode = new_inode(dir->i_sb);
		if (!inode)
			return ERR_PTR(-ENOMEM);
		inode->i_ino = fe->index + 10;
		inode_init_owner(&nop_mnt_idmap, inode, dir, S_IFREG | 0644);
		i_size_write(inode, (loff_t)fe->sectors * BNKFS_SECTOR_SIZE);
		inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
		inode->i_fop = &bnkfs_file_ops;
		inode->i_private = fe;
	}
	d_add(dentry, inode);
	return NULL;
}

static const struct inode_operations bnkfs_dir_inode_ops = {
	.lookup = bnkfs_lookup,
};

static const struct file_operations bnkfs_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = bnkfs_iterate,
	.llseek = generic_file_llseek,
};

static int bnkfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);
	u64 total = le64_to_cpu(sbi->dsb.total_sectors);

	buf->f_type = BNKFS_MAGIC;
	buf->f_bsize = BNKFS_SECTOR_SIZE;
	buf->f_blocks = total;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_files = sbi->file_count + 1;
	buf->f_ffree = 0;
	buf->f_namelen = sbi->max_name_len;
	return 0;
}

static void bnkfs_put_super(struct super_block *sb)
{
	struct bnkfs_sb_info *sbi = BNKFS_SB(sb);

	if (!sbi)
		return;

	mutex_lock(&bnkfs_ctl_lock);
	if (bnkfs_active_sbi == sbi)
		bnkfs_active_sbi = NULL;
	mutex_unlock(&bnkfs_ctl_lock);

	kfree(sbi->files);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static const struct super_operations bnkfs_super_ops = {
	.statfs = bnkfs_statfs,
	.put_super = bnkfs_put_super,
};

static bool bnkfs_device_matches(struct super_block *sb)
{
	char short_name[BDEVNAME_SIZE];
	const char *base = disk_name;
	const char *slash;

	if (!disk_name || !disk_name[0])
		return true;

	bdevname(sb->s_bdev, short_name);
	slash = strrchr(base, '/');
	if (slash)
		base = slash + 1;
	return !strcmp(short_name, base);
}

static int bnkfs_prepare_super(struct super_block *sb, struct bnkfs_sb_info *sbi)
{
	struct bnkfs_disk_super a = { 0 }, b = { 0 };
	bool va = false, vb = false;
	bool format_new = false;
	u64 total_sectors;
	u64 data_start;
	int ret;

	ret = bnkfs_read_disk_super(sb, sbi->primary_sector, &a);
	if (!ret)
		va = bnkfs_super_valid(&a);
	ret = bnkfs_read_disk_super(sb, sbi->backup_sector, &b);
	if (!ret)
		vb = bnkfs_super_valid(&b);

	if (va) {
		sbi->dsb = a;
		if (!vb)
			return bnkfs_write_disk_super(sb, sbi);
		return 0;
	}
	if (vb) {
		sbi->dsb = b;
		return bnkfs_write_disk_super(sb, sbi);
	}

	total_sectors = i_size_read(sb->s_bdev->bd_inode) >> 9;
	data_start = max_t(u64, sbi->primary_sector, sbi->backup_sector) + 1;
	if (total_sectors <= data_start)
		return -ENOSPC;

	memset(&sbi->dsb, 0, sizeof(sbi->dsb));
	sbi->dsb.magic = cpu_to_le32(BNKFS_MAGIC);
	sbi->dsb.version = cpu_to_le32(BNKFS_VERSION);
	sbi->dsb.primary_sector = cpu_to_le64(sbi->primary_sector);
	sbi->dsb.backup_sector = cpu_to_le64(sbi->backup_sector);
	sbi->dsb.total_sectors = cpu_to_le64(total_sectors);
	sbi->dsb.data_start_sector = cpu_to_le64(data_start);
	sbi->dsb.file_sectors = cpu_to_le32(sbi->file_sectors);
	sbi->dsb.max_name_len = cpu_to_le32(sbi->max_name_len);
	sbi->dsb.file_count = cpu_to_le32((total_sectors - data_start) / sbi->file_sectors);
	format_new = true;

	ret = bnkfs_write_disk_super(sb, sbi);
	if (ret)
		return ret;
	return format_new ? 1 : 0;
}

static int bnkfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	(void)fc;
	struct inode *root_inode;
	struct dentry *root;
	struct bnkfs_sb_info *sbi;
	int prep;
	int ret;

	if (!bnkfs_device_matches(sb))
		return -EINVAL;

	ret = sb_set_blocksize(sb, BNKFS_SECTOR_SIZE);
	if (!ret)
		return -EINVAL;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	mutex_init(&sbi->io_lock);
	sbi->sb = sb;
	sbi->primary_sector = sb_primary_sector;
	sbi->backup_sector = sb_backup_sector;
	sbi->max_name_len = clamp_t(u32, max_filename_len, 6, BNKFS_IOCTL_NAME_MAX - 1);
	sbi->file_sectors = max_t(u32, 1, max_file_sectors);

	sb->s_magic = BNKFS_MAGIC;
	sb->s_op = &bnkfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_fs_info = sbi;

	prep = bnkfs_prepare_super(sb, sbi);
	if (prep < 0) {
		ret = prep;
		goto err;
	}

	sbi->file_count = le32_to_cpu(sbi->dsb.file_count);
	sbi->file_sectors = le32_to_cpu(sbi->dsb.file_sectors);
	sbi->max_name_len = le32_to_cpu(sbi->dsb.max_name_len);
	if (!sbi->file_count || !sbi->file_sectors || sbi->max_name_len < 6) {
		ret = -EINVAL;
		goto err;
	}

	sbi->files = kcalloc(sbi->file_count, sizeof(*sbi->files), GFP_KERNEL);
	if (!sbi->files) {
		ret = -ENOMEM;
		goto err;
	}

	ret = bnkfs_fill_entries(sb, sbi, prep == 1);
	if (ret)
		goto err;

	root_inode = new_inode(sb);
	if (!root_inode) {
		ret = -ENOMEM;
		goto err;
	}
	root_inode->i_ino = 1;
	inode_init_owner(&nop_mnt_idmap, root_inode, NULL, S_IFDIR | 0555);
	root_inode->i_fop = &bnkfs_dir_ops;
	root_inode->i_op = &bnkfs_dir_inode_ops;
	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);
	set_nlink(root_inode, 2);

	root = d_make_root(root_inode);
	if (!root) {
		ret = -ENOMEM;
		goto err;
	}
	sb->s_root = root;

	mutex_lock(&bnkfs_ctl_lock);
	if (bnkfs_active_sbi) {
		mutex_unlock(&bnkfs_ctl_lock);
		ret = -EBUSY;
		goto err;
	}
	bnkfs_active_sbi = sbi;
	mutex_unlock(&bnkfs_ctl_lock);

	return 0;

err:
	bnkfs_put_super(sb);
	return ret;
}

static int bnkfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, bnkfs_fill_super);
}

static void bnkfs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
}

static const struct fs_context_operations bnkfs_context_ops = {
	.get_tree = bnkfs_get_tree,
};

static int bnkfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &bnkfs_context_ops;
	return 0;
}

static struct file_system_type bnkfs_fs_type = {
	.owner = THIS_MODULE,
	.name = BNKFS_NAME,
	.init_fs_context = bnkfs_init_fs_context,
	.kill_sb = bnkfs_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,
};

static long bnkfs_ctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct bnkfs_sb_info *sbi;
	long ret = 0;

	(void)file;

	mutex_lock(&bnkfs_ctl_lock);
	sbi = bnkfs_active_sbi;
	if (!sbi) {
		mutex_unlock(&bnkfs_ctl_lock);
		return -ENODEV;
	}
	mutex_unlock(&bnkfs_ctl_lock);

	mutex_lock(&sbi->io_lock);

	switch (cmd) {
	case BNKFS_IOC_ZERO_ALL: {
		u32 i;
		for (i = 0; i < sbi->file_count; i++) {
			ret = bnkfs_zero_file(sbi->sb, &sbi->files[i]);
			if (ret)
				break;
		}
		break;
	}
	case BNKFS_IOC_ERASE_FS: {
		u8 zeros[BNKFS_SECTOR_SIZE];
		u64 i;
		u64 total = le64_to_cpu(sbi->dsb.total_sectors);

		memset(zeros, 0, sizeof(zeros));
		for (i = 0; i < total; i++) {
			ret = bnkfs_write_raw_sector(sbi->sb, i, zeros);
			if (ret)
				break;
		}
		if (!ret) {
			u32 k;
			for (k = 0; k < sbi->file_count; k++)
				sbi->files[k].hash = 0;
		}
		break;
	}
	case BNKFS_IOC_GET_HASHES: {
		struct bnkfs_ioctl_hash_list req;
		struct bnkfs_ioctl_hash_entry *entries;
		u32 i, count;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}

		req.total_count = sbi->file_count;
		count = min(req.capacity, sbi->file_count);
		req.count = count;

		entries = kcalloc(count, sizeof(*entries), GFP_KERNEL);
		if (!entries) {
			ret = -ENOMEM;
			break;
		}

		for (i = 0; i < count; i++) {
			strscpy(entries[i].name, sbi->files[i].name, sizeof(entries[i].name));
			entries[i].start_sector = sbi->files[i].start_sector;
			entries[i].sectors = sbi->files[i].sectors;
			entries[i].hash = sbi->files[i].hash;
		}

		if (count && copy_to_user((void __user *)(unsigned long)req.entries_ptr, entries,
					  count * sizeof(*entries)))
			ret = -EFAULT;

		kfree(entries);
		if (!ret && copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		break;
	}
	case BNKFS_IOC_GET_MAP: {
		struct bnkfs_ioctl_sector_map_req req;
		struct bnkfs_file_entry *fe;
		size_t nlen;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		req.name[BNKFS_IOCTL_NAME_MAX - 1] = '\0';
		nlen = strnlen(req.name, BNKFS_IOCTL_NAME_MAX);
		fe = bnkfs_find_file(sbi, req.name, nlen);
		if (!fe) {
			ret = -ENOENT;
			break;
		}
		req.start_sector = fe->start_sector;
		req.sectors = fe->sectors;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		break;
	}
	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&sbi->io_lock);
	return ret;
}

static const struct file_operations bnkfs_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bnkfs_ctl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = bnkfs_ctl_ioctl,
#endif
};

static struct miscdevice bnkfs_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "bnkfs_ctl",
	.fops = &bnkfs_ctl_fops,
	.mode = 0600,
};

static int __init bnkfs_init(void)
{
	int ret;

	ret = misc_register(&bnkfs_miscdev);
	if (ret)
		return ret;

	ret = register_filesystem(&bnkfs_fs_type);
	if (ret) {
		misc_deregister(&bnkfs_miscdev);
		return ret;
	}

	pr_info("bnkfs: loaded\n");
	return 0;
}

static void __exit bnkfs_exit(void)
{
	unregister_filesystem(&bnkfs_fs_type);
	misc_deregister(&bnkfs_miscdev);
	pr_info("bnkfs: unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("Simple educational FS with duplicated superblock");

module_init(bnkfs_init);
module_exit(bnkfs_exit);
