#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>

#include "internal.h"

static char *disk_name = "";
module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "expected backing device short name");

static unsigned int sb1_offset = 0;
module_param(sb1_offset, uint, 0444);
MODULE_PARM_DESC(sb1_offset, "primary superblock sector");

static unsigned int sb2_offset = 16;
module_param(sb2_offset, uint, 0444);
MODULE_PARM_DESC(sb2_offset, "secondary superblock sector");

static unsigned int max_name_len = 32;
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "maximum filename length");

static unsigned int max_file_sectors = 4;
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "file size in sectors (M)");

unsigned int bfs_param_sb1(void)        { return sb1_offset; }
unsigned int bfs_param_sb2(void)        { return sb2_offset; }
unsigned int bfs_param_maxname(void)    { return max_name_len; }
unsigned int bfs_param_filespan(void)   { return max_file_sectors; }
const char  *bfs_param_diskname(void)   { return disk_name; }

static int bfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, bfs_fill_root_super);
}

static void bfs_free_fc(struct fs_context *fc) { }

static const struct fs_context_operations bfs_ctx_ops = {
	.get_tree = bfs_get_tree,
	.free     = bfs_free_fc,
};

static int bfs_init_ctx(struct fs_context *fc)
{
	fc->ops = &bfs_ctx_ops;
	return 0;
}

static void bfs_kill(struct super_block *sb)
{
	if (sb->s_bdev)
		sync_blockdev(sb->s_bdev);
	kill_block_super(sb);
}

static struct file_system_type bfs_type = {
	.owner            = THIS_MODULE,
	.name             = BFS_NAME,
	.init_fs_context  = bfs_init_ctx,
	.kill_sb          = bfs_kill,
	.fs_flags         = FS_REQUIRES_DEV,
};

static int __init bfs_mod_init(void)
{
	int rc = register_filesystem(&bfs_type);

	if (rc) {
		pr_err("bnkfs: register_filesystem() = %d\n", rc);
		return rc;
	}
	pr_info("bnkfs: загружен. Параметры: disk_name=%s sb1=%u sb2=%u maxname=%u M=%u\n",
		disk_name, sb1_offset, sb2_offset, max_name_len, max_file_sectors);
	return 0;
}

static void __exit bfs_mod_exit(void)
{
	unregister_filesystem(&bfs_type);
	pr_info("bnkfs: выгружен\n");
}

module_init(bfs_mod_init);
module_exit(bfs_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bunkevich F.S.");
MODULE_DESCRIPTION("BunkevichFS");
MODULE_VERSION("1.0");
