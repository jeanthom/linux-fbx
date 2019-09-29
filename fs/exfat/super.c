/*
 * super.c<2> for exfat
 * Created by <nschichan@freebox.fr> on Tue Jul 23 12:33:53 2013
 */

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/cred.h>

#include "exfat_fs.h"
#include "exfat.h"


#define PFX	"exFAT: "

static void exfat_put_super(struct super_block *sb);
static int exfat_statfs(struct dentry *dentry, struct kstatfs *kstat);
static int exfat_show_options(struct seq_file *m, struct dentry *root);
static int exfat_remount(struct super_block *sb, int *flags, char *opts);

static const struct super_operations exfat_super_ops = {
	.alloc_inode	= exfat_alloc_inode,
	.destroy_inode	= exfat_destroy_inode,
	.drop_inode	= exfat_drop_inode,
	.evict_inode	= exfat_evict_inode,
	.write_inode	= exfat_write_inode,
	.statfs         = exfat_statfs,
	.put_super      = exfat_put_super,
	.show_options	= exfat_show_options,
	.remount_fs	= exfat_remount,
};

const struct file_operations exfat_dir_operations = {
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.iterate = exfat_iterate,
	.unlocked_ioctl	= exfat_ioctl,
};

const struct file_operations exfat_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.splice_read	= generic_file_splice_read,
	.unlocked_ioctl	= exfat_ioctl,
	.fsync		= generic_file_fsync,
};

const struct inode_operations exfat_dir_inode_operations =
{
	.create = exfat_inode_create,
	.mkdir	= exfat_inode_mkdir,
	.lookup = exfat_inode_lookup,
	.rmdir	= exfat_inode_rmdir,
	.unlink	= exfat_inode_unlink,
	.rename	= exfat_rename,
	.setattr = exfat_setattr,
	.getattr = exfat_getattr,
};

const struct inode_operations exfat_file_inode_operations = {
	.setattr = exfat_setattr,
	.getattr = exfat_getattr,
};

const struct address_space_operations exfat_address_space_operations = {
	.readpage	= exfat_readpage,
	.readpages	= exfat_readpages,
	.write_begin	= exfat_write_begin,
	.write_end	= exfat_write_end,
	.writepage	= exfat_writepage,
	.writepages	= exfat_writepages,
};

void exfat_msg(struct super_block *sb, const char *prefix,
		const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sexFAT-fs (%s): %pV\n", prefix, sb->s_id, &vaf);
	va_end(args);
}

void exfat_fs_error(struct super_block *sb, const char *fmt, ...)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	exfat_msg(sb, KERN_ERR, "error: %pV", &vaf);
	va_end(args);

	if (sbi->options.error_action == EXFAT_ERROR_ACTION_REMOUNT_RO &&
	    !(sb->s_flags & MS_RDONLY)) {
		sb->s_flags |= MS_RDONLY;
		exfat_msg(sb, KERN_ERR, "remounted read-only due to fs error.");
	} else if (sbi->options.error_action == EXFAT_ERROR_ACTION_PANIC)
		panic("exFAT-fs (%s): panic due fs error.\n", sb->s_id);
}

/*
 * process checksum on buffer head. first indicates if the special
 * treatment of the first sector needs to be done or not.
 *
 * first sector can be changed (volume flags, and heap use percent),
 * those fields are excluded from the checksum to allow updating
 * without recalculating the checksum.
 */
static u32 exfat_sb_checksum_process(struct buffer_head *bh, u32 checksum,
				     unsigned int size,
				     bool first)
{
	unsigned int i;

	for (i = 0; i < size; ++i) {
		if (first && (i == 106 || i == 107 || i == 112))
			continue ;
		checksum = ((checksum << 31) | (checksum >> 1)) +
			(unsigned char)bh->b_data[i];
	}
	return checksum;
}

static int exfat_check_sb_checksum(struct super_block *sb)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	u32 checksum;
	int i;
	int err;
	struct buffer_head *bh[EXFAT_CHECKSUM_SECTORS + 1];

	/*
	 * fetch needed sectors, reuse first sector from sbi.
	 */
	err = -ENOMEM;
	memset(bh, 0, sizeof (struct buffer_head*) *
	       (EXFAT_CHECKSUM_SECTORS + 1));
	bh[0] = sbi->sb_bh;
	for (i = 1; i < EXFAT_CHECKSUM_SECTORS + 1; ++i) {
		bh[i] = sb_bread(sb, i);
		if (!bh[i])
			goto out;
	}

	/*
	 * calculate checksum.
	 */
	checksum = exfat_sb_checksum_process(bh[0], 0, sbi->sectorsize, true);
	for (i = 1; i < EXFAT_CHECKSUM_SECTORS; ++i) {
		checksum = exfat_sb_checksum_process(bh[i], checksum,
						     sbi->sectorsize, false);
	}

	/*
	 * compare with the checksum sector.
	 */
	err = -EINVAL;
	for (i = 0; i < sbi->sectorsize; i += sizeof (u32)) {
		__le32 val = *(u32*)(bh[EXFAT_CHECKSUM_SECTORS]->b_data + i);

		if (__le32_to_cpu(val) != checksum) {
			exfat_msg(sb, KERN_INFO, "at offset %i, checksum "
				  "%08x != %08x", i, __le32_to_cpu(val), checksum);
			goto out;
		}
	}
	err = 0;

out:
	for (i = 1; i < EXFAT_CHECKSUM_SECTORS; ++i)
		if (bh[i])
			brelse(bh[i]);
	return err;
}

static int exfat_check_sb(struct super_block *sb)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_vbr *vbr = sbi->vbr;
	u16 fs_rev;
	u16 flags;
	int active_fat;
	u16 num_fats;

	if (memcmp(vbr->jump, "\xeb\x76\x90", sizeof (vbr->jump))) {
		exfat_msg(sb, KERN_INFO, "invalid jump field in vbr.");
		return -EINVAL;
	}

	if (memcmp(vbr->fsname, "EXFAT   ", 8)) {
		exfat_msg(sb, KERN_INFO, "invalid fsname field in vbr: %s.",
			  vbr->fsname);
		return -EINVAL;
	}

	fs_rev = __le16_to_cpu(vbr->fs_rev);
	if (fs_rev != 0x0100) {
		exfat_msg(sb, KERN_INFO, "filesystem version invalid: "
			  "have 0x%04x, need 0x0100", fs_rev);
		return -EINVAL;
	}

	flags = __le16_to_cpu(vbr->volume_flags);
	active_fat = exfat_active_fat(flags);
	if (active_fat != 0) {
		exfat_msg(sb, KERN_INFO, "filesystems with active fat > 0 are "
			  "not supported.");
		return -EINVAL;
	}

	if (flags & EXFAT_FLAG_MEDIA_FAILURE)
		exfat_msg(sb, KERN_WARNING, "filesystem had media failure(s)");

	/*
	 * bytes per sectors are on the range 2^9 - 2^12 (512 - 4096)
	 */
	if (vbr->bytes_per_sector < 9 || vbr->bytes_per_sector > 12) {
		exfat_msg(sb, KERN_ERR, "invalid byte per sectors: %u",
			  (1 << vbr->bytes_per_sector));
		return -EINVAL;
	}

	/*
	 * sectors per cluster can be as low as 0, and must not result
	 * in a cluster size higher than 32MB (byte_per_sector +
	 * sectors_per_cluster must not be creater than 25)
	 */
	if (vbr->bytes_per_sector + vbr->sectors_per_cluster > 25) {
		exfat_msg(sb, KERN_ERR, "invalid cluster size: %u",
		  1 << (vbr->bytes_per_sector + vbr->sectors_per_cluster));
		return -EINVAL;
	}

	num_fats = __le16_to_cpu(vbr->fat_num);
	if (num_fats == 0) {
		exfat_msg(sb, KERN_ERR, "superblock reports no FAT.");
		return -EINVAL;
	}
	if (num_fats > 1) {
		exfat_msg(sb, KERN_ERR, "TexFAT is not supported.");
		return -EINVAL;
	}

	if (memcmp(vbr->boot_sig, "\x55\xaa", 2)) {
		exfat_msg(sb, KERN_ERR, "invalid end boot signature: %02x%02x.",
			  vbr->boot_sig[0], vbr->boot_sig[1]);
		return -EINVAL;
	}

	return 0;
}

static int exfat_fill_root(struct super_block *sb, struct inode *root)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	u32 nclust;
	u32 dummy;
	loff_t links;

	root->i_ino = EXFAT_ROOT_INO;
	root->i_version = 1;
	EXFAT_I(root)->first_cluster =
		__le32_to_cpu(sbi->root_dir_cluster);
	EXFAT_I(root)->attributes = E_EXFAT_ATTR_DIRECTORY;

	root->i_uid = sbi->options.uid;
	root->i_gid = sbi->options.gid;

	root->i_mode = exfat_make_mode(sbi, S_IRWXUGO, E_EXFAT_ATTR_DIRECTORY);
	root->i_version++;
	root->i_generation = 0;

	root->i_op = &exfat_dir_inode_operations;
	root->i_fop = &exfat_dir_operations;

	/*
	 * root inode cannot use bitmap.
	 */
	EXFAT_I(root)->flags = EXFAT_I_ALLOC_POSSIBLE;

	/*
	 * set i_size
	 */
	nclust = 0;
	while (__exfat_get_fat_cluster(root, nclust, &dummy, false) == 0)
		++nclust;
	root->i_size = nclust << sbi->clusterbits;
	root->i_blocks = nclust << (sbi->clusterbits - 9);
	EXFAT_I(root)->allocated_clusters = nclust;

	/*
	 * +2 to account for '.' and '..'
	 */
	links = exfat_dir_links(root);
	if (links < 0)
		return links;
	set_nlink(root, links + 2);

	root->i_mtime = root->i_atime = root->i_ctime = CURRENT_TIME_SEC;

	return 0;
}

static loff_t exfat_file_max_byte(struct exfat_sb_info *sbi)
{
	u32 max_clusters = EXFAT_CLUSTER_LASTVALID -
		EXFAT_CLUSTER_FIRSTVALID + 1;

	return (loff_t)max_clusters << sbi->clusterbits;
}

static int exfat_show_options(struct seq_file *m, struct dentry *root)
{
	struct exfat_sb_info *sbi = EXFAT_SB(root->d_inode->i_sb);

	if (!uid_eq(sbi->options.uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, sbi->options.uid));
	if (!gid_eq(sbi->options.gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, sbi->options.gid));

	seq_printf(m, ",fmask=%04o", sbi->options.fmask);
	seq_printf(m, ",dmask=%04o", sbi->options.dmask);

	if (sbi->options.time_offset_set)
		seq_printf(m, ",time_offset=%d", sbi->options.time_offset);

	switch (sbi->options.error_action) {
	case EXFAT_ERROR_ACTION_PANIC:
		seq_printf(m, ",errors=panic");
		break;
	case EXFAT_ERROR_ACTION_REMOUNT_RO:
		seq_printf(m, ",errors=remount-ro");
		break;
	default:
		seq_printf(m, ",errors=continue");
		break;
	}

	return 0;
}

enum {
	Opt_exfat_uid,
	Opt_exfat_gid,
	Opt_exfat_dmask,
	Opt_exfat_fmask,
	Opt_exfat_time_offset,
	Opt_exfat_error_continue,
	Opt_exfat_error_remount_ro,
	Opt_exfat_error_panic,
	Opt_exfat_err,
};

static const match_table_t exfat_tokens = {
	{ Opt_exfat_uid, "uid=%u", },
	{ Opt_exfat_gid, "gid=%u", },
	{ Opt_exfat_dmask, "dmask=%04o", },
	{ Opt_exfat_fmask, "fmask=%04o", },
	{ Opt_exfat_time_offset, "time_offset=%d", },
	{ Opt_exfat_error_continue, "errors=continue", },
	{ Opt_exfat_error_remount_ro, "errors=remount-ro", },
	{ Opt_exfat_error_panic, "errors=panic", },
	{ Opt_exfat_err, NULL },
};

static int exfat_parse_options(struct super_block *sb, char *opts, int silent)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	char *p;

	sbi->options.uid = current_uid();
	sbi->options.gid = current_gid();

	sbi->options.dmask = current_umask();
	sbi->options.fmask = current_umask();
	sbi->options.time_offset_set = 0;
	sbi->options.error_action = EXFAT_ERROR_ACTION_CONTINUE;

	while (1) {
		int token;
		substring_t args[MAX_OPT_ARGS];
		unsigned int optval;

		p = strsep(&opts, ",");
		if (!p)
			break;
		token = match_token(p, exfat_tokens, args);

		switch (token) {
		case Opt_exfat_uid:
			if (match_int(&args[0], &optval))
				return -EINVAL;
			sbi->options.uid = make_kuid(current_user_ns(), optval);
			break;

		case Opt_exfat_gid:
			if (match_int(&args[0], &optval))
				return -EINVAL;
			sbi->options.gid = make_kgid(current_user_ns(), optval);
			break;

		case Opt_exfat_dmask:
			if (match_octal(&args[0], &optval))
				return -EINVAL;
			sbi->options.dmask = optval;
			break;

		case Opt_exfat_fmask:
			if (match_octal(&args[0], &optval))
				return -EINVAL;
			sbi->options.fmask = optval;
			break;

		case Opt_exfat_time_offset:
			if (match_int(&args[0], &optval))
				return -EINVAL;
			if (optval < -12 * 60 && optval > 12 * 60) {
				if (!silent)
					exfat_msg(sb, KERN_INFO, "invalid "
						  "time_offset value %d: "
						  "should be between %d and %d",
						  optval, -12 * 60, 12 * 60);
				return -EINVAL;
			}
			sbi->options.time_offset = optval;
			sbi->options.time_offset_set = 1;
			break;

		case Opt_exfat_error_continue:
			sbi->options.error_action = EXFAT_ERROR_ACTION_CONTINUE;
			break;

		case Opt_exfat_error_remount_ro:
			sbi->options.error_action =
				EXFAT_ERROR_ACTION_REMOUNT_RO;
			break;

		case Opt_exfat_error_panic:
			sbi->options.error_action = EXFAT_ERROR_ACTION_PANIC;
			break;

		default:
			if (!silent)
				exfat_msg(sb, KERN_INFO, "Unrecognized mount "
					  "option %s or missing parameter.\n",
					  p);
			return -EINVAL;
		}
	}
	return 0;
}

static void exfat_set_sb_dirty(struct super_block *sb, bool set, bool force)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	u16 flags;

	/*
	 * do not change anything if mounted read only and not
	 * forced. the force case would happen during remount.
	 */
	if ((sb->s_flags & MS_RDONLY) && !force)
		return ;

	if (sbi->dirty) {
		if (set)
			exfat_msg(sb, KERN_WARNING, "Volume was not cleanly "
				  "umounted. fsck should probably be needed.");
		return ;
	}

	flags = __le16_to_cpu(sbi->vbr->volume_flags);
	if (set)
		flags |= EXFAT_FLAG_DIRTY;
	else
		flags &= ~EXFAT_FLAG_DIRTY;
	sbi->vbr->volume_flags = __cpu_to_le16(flags);

	mark_buffer_dirty(sbi->sb_bh);
	sync_dirty_buffer(sbi->sb_bh);
}

static int exfat_remount(struct super_block *sb, int *flags, char *opts)
{
	int new_rdonly = *flags & MS_RDONLY;

	if (new_rdonly != (sb->s_flags & MS_RDONLY)) {
		if (new_rdonly)
			exfat_set_sb_dirty(sb, false, false);
		else
			/*
			 * sb->s_flag still has MS_RDONLY, so we need
			 * to force the dirty state
			 */
			exfat_set_sb_dirty(sb, true, true);
	}
	return 0;
}

static int exfat_fill_super(struct super_block *sb, void *data, int silent)
{
	struct exfat_sb_info *sbi = NULL;
	int ret = -ENOMEM;
	struct inode *root = NULL;
	int i;

	sbi = kzalloc(sizeof (*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	if (exfat_parse_options(sb, data, silent) < 0)
		return -EINVAL;

	mutex_init(&sbi->sb_mutex);
	spin_lock_init(&sbi->inode_hash_lock);

	/*
	 * first block, before we know sector size.
	 */
	sbi->sb_bh = sb_bread(sb, 0);
	if (!sbi->sb_bh)
		goto fail;

	sbi->vbr = (struct exfat_vbr*)sbi->sb_bh->b_data;
	sb->s_op = &exfat_super_ops;


	ret = exfat_check_sb(sb);
	if (ret)
		goto fail;

	/*
	 * vbr seems sane, fill sbi.
	 */
	sbi->sectorsize = (1 << sbi->vbr->bytes_per_sector);
	sbi->clustersize = sbi->sectorsize *
		(1 << sbi->vbr->sectors_per_cluster);

	sbi->sectors_per_cluster = sbi->clustersize / sbi->sectorsize;

	sbi->sectorbits = sbi->vbr->bytes_per_sector;
	sbi->clusterbits = sbi->vbr->sectors_per_cluster + sbi->sectorbits;
	sbi->sectormask = sbi->sectorsize - 1;
	sbi->clustermask = sbi->clustersize - 1;


	sbi->fat_offset = __le32_to_cpu(sbi->vbr->fat_offset);
	sbi->fat_length = __le32_to_cpu(sbi->vbr->fat_length);

	sbi->root_dir_cluster = __le32_to_cpu(sbi->vbr->cluster_root_dir);

	sbi->cluster_heap_offset = __le32_to_cpu(sbi->vbr->cluster_heap_offset);
	sbi->cluster_count = __le32_to_cpu(sbi->vbr->cluster_count);

	sbi->dirty = !!(__le16_to_cpu(sbi->vbr->volume_flags) &
			EXFAT_FLAG_DIRTY);

	/*
	 * now that we know sector size, reread superblock with
	 * correct sector size.
	 */
	ret = -EIO;
	if (sb->s_blocksize != sbi->sectorsize) {
		if (!sb_set_blocksize(sb, sbi->sectorsize)) {
			exfat_msg(sb, KERN_INFO, "bad block size %d.",
				  sbi->sectorsize);
			goto fail;
		}

		brelse(sbi->sb_bh);
		sbi->vbr = NULL;

		sbi->sb_bh = sb_bread(sb, 0);
		if (!sbi->sb_bh)
			goto fail;
		sbi->vbr = (struct exfat_vbr*)sbi->sb_bh->b_data;
		sb->s_fs_info = sbi;
	}

	ret = exfat_check_sb_checksum(sb);
	if (ret)
		goto fail;

	sb->s_maxbytes = exfat_file_max_byte(sbi);

	ret = exfat_init_fat(sb);
	if (ret)
		goto fail;

	for (i = 0 ; i < EXFAT_HASH_SIZE; ++i) {
		INIT_HLIST_HEAD(&sbi->inode_hash[i]);
	}

	/*
	 * create root inode.
	 */
	root = new_inode(sb);
	if (!root)
		goto fail;

	exfat_fill_root(sb, root);

	ret = exfat_upcase_init(root);
	if (ret)
		goto fail_iput;

	ret = exfat_init_bitmap(root);
	if (ret)
		goto fail_iput;


	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		goto fail_iput;

	exfat_set_sb_dirty(sb, true, false);
	return 0;

fail_iput:
	iput(root);

fail:
	if (sbi->sb_bh)
		brelse(sbi->sb_bh);
	if (sbi)
		kfree(sbi);
	return ret;
}

static struct dentry *exfat_mount(struct file_system_type *fstype,
				  int flags, const char *dev_name, void *data)
{
	return mount_bdev(fstype, flags, dev_name, data, exfat_fill_super);
}

static void exfat_put_super(struct super_block *sb)
{
	struct exfat_sb_info *sbi;

	sbi = EXFAT_SB(sb);
	if (sbi) {
		exfat_set_sb_dirty(sb, false, false);
		exfat_exit_bitmap(sb);
		brelse(sbi->sb_bh);
		kfree(sbi->upcase_table);
		kfree(sbi);
	}
}

static int exfat_statfs(struct dentry *dentry, struct kstatfs *kstat)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	memset(kstat, 0, sizeof (*kstat));


	kstat->f_bsize = sbi->clustersize;
	kstat->f_blocks = sbi->cluster_count;
	kstat->f_bfree = sbi->free_clusters;
	kstat->f_bavail = sbi->free_clusters;
	kstat->f_namelen = 255;
	kstat->f_fsid.val[0] = (u32)id;
	kstat->f_fsid.val[1] = (u32)(id >> 32);

	return 0;
}

static struct file_system_type exfat_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "exfat",
	.mount		= exfat_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init exfat_init(void)
{
	int error;

	/* some sanity check on internal structure sizes */
	BUILD_BUG_ON(sizeof (struct exfat_vbr) != 512);

	BUILD_BUG_ON(sizeof (struct exfat_volume_label_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_bitmap_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_upcase_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_guid_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_padding_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_acl_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_filedir_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_stream_extension_entry) != 0x20);
	BUILD_BUG_ON(sizeof (struct exfat_filename_entry) != 0x20);

	error = exfat_init_inodes();
	if (error)
		return error;


	error = register_filesystem(&exfat_fs_type);
	if (error)
		exfat_exit_inodes();
	return error;
}

static void __exit exfat_exit(void)
{
	unregister_filesystem(&exfat_fs_type);
	exfat_exit_inodes();
}

module_init(exfat_init);
module_exit(exfat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicolas Schichan <nschichan@freebox.fr>");
