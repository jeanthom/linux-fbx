/*
 * inode.c<2> for exfat
 * Created by <nschichan@freebox.fr> on Wed Jul 24 16:15:52 2013
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/hash.h>

#include "exfat_fs.h"
#include "exfat.h"

static struct kmem_cache *exfat_inodes_cachep;

/*
 * inode callbacks.
 */
struct inode *exfat_alloc_inode(struct super_block *sb)
{
	struct exfat_inode_info *ei = kmem_cache_alloc(exfat_inodes_cachep,
						       GFP_NOFS);

	if (!ei)
		return NULL;

	return &ei->vfs_inode;
}

static void exfat_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(exfat_inodes_cachep, EXFAT_I(inode));
}

void exfat_destroy_inode(struct inode *_inode)
{
	struct exfat_inode_info *inode = EXFAT_I(_inode);

	call_rcu(&inode->vfs_inode.i_rcu, exfat_i_callback);
}

static void exfat_inode_init_once(void *ptr)
{
	struct exfat_inode_info *info = ptr;

	INIT_HLIST_NODE(&info->hash_list);
	exfat_inode_cache_init(&info->vfs_inode);
	inode_init_once(&info->vfs_inode);
}

/*
 * inode cache create/destroy.
 */
int exfat_init_inodes(void)
{
	exfat_inodes_cachep = kmem_cache_create("exfat-inodes",
				       sizeof (struct exfat_inode_info), 0,
				       SLAB_RECLAIM_ACCOUNT |SLAB_MEM_SPREAD,
				       exfat_inode_init_once);
	if (!exfat_inodes_cachep)
		return -ENOMEM;
	return 0;
}

void exfat_exit_inodes(void)
{
	kmem_cache_destroy(exfat_inodes_cachep);
}

int exfat_drop_inode(struct inode *inode)
{
	return generic_drop_inode(inode);
}

void exfat_evict_inode(struct inode *inode)
{
	if (inode->i_data.nrpages)
		truncate_inode_pages(&inode->i_data, 0);
	if (!inode->i_nlink) {
		inode->i_size = 0;
		exfat_free_clusters_inode(inode, 0);
	}
	invalidate_inode_buffers(inode);
	clear_inode(inode);
	exfat_remove_inode_hash(inode);
	exfat_inode_cache_drop(inode);
}

static u32 exfat_hash(loff_t disk_pos)
{
	return hash_32(disk_pos, EXFAT_HASH_BITS);
}

struct inode *exfat_iget(struct super_block *sb, loff_t disk_pos)
{
	struct exfat_inode_info *info;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct hlist_head *head = sbi->inode_hash + exfat_hash(disk_pos);
	struct inode *ret = NULL;


	spin_lock(&sbi->inode_hash_lock);
	hlist_for_each_entry (info, head, hash_list) {
		if (info->iloc.disk_offs[0] != disk_pos)
			continue ;
		ret = igrab(&info->vfs_inode);
		if (ret)
			break;
	}
	spin_unlock(&sbi->inode_hash_lock);
	return ret;
}

void exfat_insert_inode_hash(struct inode *inode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct hlist_head *head = sbi->inode_hash +
		exfat_hash(info->iloc.disk_offs[0]);

	spin_lock(&sbi->inode_hash_lock);
	hlist_add_head(&info->hash_list, head);
	spin_unlock(&sbi->inode_hash_lock);
}

void exfat_remove_inode_hash(struct inode *inode)
{
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);

	spin_lock(&sbi->inode_hash_lock);
	info->iloc.disk_offs[0] = 0;
	hlist_del_init(&info->hash_list);
	spin_unlock(&sbi->inode_hash_lock);
}

/*
 * calculate the number of links in a directory. this is the number of
 * EXFAT_FILEDIR_ENTRY typed elements in the directory stream. This
 * does not include the '.' and '..' entries.
 */
loff_t exfat_dir_links(struct inode *inode)
{
	size_t ret = 0;
	struct exfat_dir_ctx dctx;
	int error;
	bool end;

	error = exfat_init_dir_ctx(inode, &dctx, 0);
	if (error)
		return error;

	error = -EIO;
	for (;;) {
		struct exfat_filedir_entry *e =
			__exfat_dentry_next(&dctx, E_EXFAT_FILEDIR, 0xff,
					    true, &end);
		if (!e) {
			if (end)
				error = 0;
			goto out;
		}
		++ret;
	}
out:
	exfat_cleanup_dir_ctx(&dctx);
	if (error)
		return error;
	return ret;
}

int exfat_get_cluster_hint(struct inode *inode, u32 *out_hint)
{
	struct exfat_inode_info *info = EXFAT_I(inode);
	int error;
	u32 first_cluster = info->first_cluster;


	if (!first_cluster) {
		/*
		 * empty file, return a cluster likely to be free.
		 */
		*out_hint = EXFAT_SB(inode->i_sb)->prev_free_cluster + 2;
		return 0;
	}

	if (info->flags & EXFAT_I_FAT_INVALID) {
		/*
		 * not fat run, all clusters are contiguous, set hint
		 * to next last file cluster.
		 */
		*out_hint = first_cluster + info->allocated_clusters;
		return 0;
	}

	/*
	 * fat run available, walk it to get the last physical cluster
	 * address and set hint to the immediate next physical
	 * cluster.
	 */
	error = exfat_get_fat_cluster(inode, info->allocated_clusters - 1,
				      out_hint);
	if (error)
		return error;
	(*out_hint)++;
	return 0;
}

int __exfat_write_inode(struct inode *inode, bool sync)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct dir_entry_buffer entries[info->iloc.nr_secondary];
	int error;
	struct exfat_filedir_entry *efd;
	struct exfat_stream_extension_entry *esx;
	u16 checksum;

	if (inode->i_ino == EXFAT_ROOT_INO)
		return 0;

	if (info->iloc.disk_offs[0] == 0) {
		/*
		 * write_inode() to unlinked inode: don't corrupt
		 * superblock.
		 */
		return 0;
	}

	error = exfat_get_dir_entry_buffers(inode, &info->iloc,
					    entries, info->iloc.nr_secondary);
	if (error)
		return error;

	if (inode->i_mode & S_IWUGO)
		info->attributes &= ~E_EXFAT_ATTR_RO;
	else
		info->attributes |= E_EXFAT_ATTR_RO;

	efd = entries[0].start;
	esx = entries[1].start;

	efd->attributes = __cpu_to_le16(info->attributes);
	esx->data_length = __cpu_to_le64(inode->i_size);
	esx->valid_data_length = esx->data_length =
		__cpu_to_le64(inode->i_size);
	esx->flags = info->flags;
	esx->first_cluster = __cpu_to_le32(info->first_cluster);

	exfat_write_time(sbi, &inode->i_ctime, &efd->create, &efd->create_10ms,
			 &efd->create_tz_offset);
	exfat_write_time(sbi, &inode->i_mtime, &efd->modified,
			 &efd->modified_10ms, &efd->modified_tz_offset);
	exfat_write_time(sbi, &inode->i_atime, &efd->accessed, NULL,
			 &efd->accessed_tz_offset);

	checksum = exfat_dir_entries_checksum(entries, info->iloc.nr_secondary);
	efd->set_checksum = __cpu_to_le16(checksum);

	exfat_dirty_dir_entries(entries, info->iloc.nr_secondary, sync);


	return 0;
}

int exfat_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;

	exfat_lock_super(inode->i_sb);
	ret = __exfat_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
	exfat_unlock_super(inode->i_sb);
	return ret;
}
