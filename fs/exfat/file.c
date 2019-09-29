/*
 * file.c for exfat
 * Created by <nschichan@freebox.fr> on Tue Aug 20 14:39:41 2013
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/exfat_user.h>

#include "exfat.h"
#include "exfat_fs.h"

static int append_fragment(struct exfat_fragment __user *ufrag,
			   struct exfat_fragment *kfrag)
{
	if (copy_to_user(ufrag, kfrag, sizeof (*kfrag)))
		return -EFAULT;
	return 0;
}

static void setup_fragment(struct exfat_sb_info *sbi,
			  struct exfat_fragment *fragment, uint32_t fcluster,
			  uint32_t dcluster)
{
	fragment->fcluster_start = fcluster;
	fragment->dcluster_start = dcluster;
	fragment->sector_start = exfat_cluster_sector(sbi, dcluster);
	fragment->nr_clusters = 1;
}

static int exfat_ioctl_get_fragments(struct inode *inode,
				     struct exfat_fragment_head __user *uhead)
{
	struct exfat_fragment_head head;
	struct exfat_fragment fragment;
	u32 fcluster;
	u32 prev_dcluster;
	u32 cur_fragment;
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	int error;

	memset(&fragment, 0, sizeof (fragment));

	if (copy_from_user(&head, uhead, sizeof (head)))
		return -EFAULT;


	if (put_user(sbi->sectorsize, &uhead->sector_size) ||
	    put_user(sbi->clustersize, &uhead->cluster_size))
		return -EFAULT;

	if (!head.nr_fragments) {
		/*
		 * user did not provide space for fragments after
		 * header.
		 */
		return 0;
	}

	if (head.fcluster_start >= info->allocated_clusters) {
		/*
		 * requested start cluster is after file EOF
		 */
		if (put_user(0, &uhead->nr_fragments))
			return -EFAULT;
		return 0;
	}

	if (info->flags & EXFAT_I_FAT_INVALID) {
		/*
		 * not FAT chain, this file has only one fragment.
		 */
		fragment.fcluster_start = head.fcluster_start;
		fragment.dcluster_start =
			info->first_cluster + head.fcluster_start;
		fragment.nr_clusters = info->allocated_clusters -
			head.fcluster_start;
		fragment.sector_start =
			exfat_cluster_sector(sbi, fragment.dcluster_start);

		if (copy_to_user(&uhead->fragments[0], &fragment,
				 sizeof (fragment)))
			return -EFAULT;
		if (put_user(1, &uhead->nr_fragments))
			return -EFAULT;
		if (put_user(info->first_cluster + info->allocated_clusters,
			     &uhead->fcluster_start))
			return -EFAULT;
		return 0;
	}

	fcluster = head.fcluster_start;
	cur_fragment = 0;

	/*
	 * initial fragment setup
	 */
	error = exfat_get_fat_cluster(inode, fcluster,
				      &prev_dcluster);
	if (error)
		return error;
	setup_fragment(sbi, &fragment, fcluster, prev_dcluster);
	++fcluster;
	while (fcluster < info->allocated_clusters) {
		int error;
		u32 dcluster;

		/*
		 * walk one step in the FAT.
		 */
		error = exfat_get_fat_cluster(inode, fcluster, &dcluster);
		if (error)
			return error;

		if (prev_dcluster == dcluster - 1) {
			/*
			 * dcluster and prev_dcluster are contiguous.
			 */
			++fragment.nr_clusters;
		} else {
			/*
			 * put this cluster in the user array
			 */
			error = append_fragment(&uhead->fragments[cur_fragment],
						&fragment);
			if (error)
				return error;

			++cur_fragment;
			if (cur_fragment == head.nr_fragments)
				break;

			/*
			 * setup a new fragment.
			 */
			setup_fragment(sbi, &fragment, fcluster, dcluster);
		}
		++fcluster;
		prev_dcluster = dcluster;
	}

	if (cur_fragment < head.nr_fragments) {
		append_fragment(&uhead->fragments[cur_fragment], &fragment);
		++cur_fragment;
	}

	/*
	 * update nr_fragments in user supplied head.
	 */
	if (cur_fragment != head.nr_fragments &&
	    put_user(cur_fragment, &uhead->nr_fragments))
		return -EFAULT;

	/*
	 * update fcluster_start in user supplied head.
	 */
	if (put_user(fcluster, &uhead->fcluster_start))
		return -EFAULT;


	return 0;
}

static int exfat_ioctl_get_bitmap(struct super_block *sb,
				  struct exfat_bitmap_head __user *uhead)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_bitmap_head head;
	uint32_t i;
	int error;
	struct exfat_bitmap_ctx ctx;
	uint32_t start_cluster;

	if (copy_from_user(&head, uhead, sizeof (head)))
		return -EFAULT;

	start_cluster = head.start_cluster;
	if (start_cluster < 2)
		return -EINVAL;


	error = exfat_init_bitmap_context(sb, &ctx, head.start_cluster);
	if (error)
		return error;
	for (i = 0; i < head.nr_entries; ++i) {
		uint32_t first_in_use;
		uint32_t nr_in_use;
		int error;

		error = exfat_test_bitmap(&ctx, start_cluster, &first_in_use,
					  &nr_in_use);
		if (error)
			goto out_error;

		if (first_in_use == sbi->cluster_count)
			break;
		if (put_user(first_in_use, &uhead->entries[i].start_cluster))
			goto out_efault;
		if (put_user(nr_in_use, &uhead->entries[i].nr_clusters))
			goto out_efault;
		if (put_user(exfat_cluster_sector(sbi, first_in_use),
			     &uhead->entries[i].sector_start))
			goto out_efault;
		if (put_user((u64)nr_in_use * sbi->sectors_per_cluster,
			     &uhead->entries[i].nr_sectors))
			goto out_efault;
		start_cluster = first_in_use + nr_in_use + 1;
	}

	exfat_exit_bitmap_context(&ctx);
	if (put_user(i, &uhead->nr_entries))
		return -EFAULT;
	if (put_user(start_cluster, &uhead->start_cluster))
		return -EFAULT;

	return 0;

out_efault:
	error = -EFAULT;
out_error:
	exfat_exit_bitmap_context(&ctx);
	return error;
}

static int exfat_ioctl_get_dirents(struct inode *inode,
				   struct exfat_dirent_head __user *uhead)
{
	struct exfat_dir_ctx dctx;
	struct exfat_dirent_head head;
	int error;
	uint32_t i;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	if (copy_from_user(&head, uhead, sizeof (head)))
		return -EFAULT;

	/* make sure we're aligned on an entry boundary */
	head.offset &= ~0x1f;

	error = exfat_init_dir_ctx(inode, &dctx, head.offset);
	if (error < 0)
		return error;

	error = 0;
	for (i = 0; i < head.nr_entries; ++i) {
		bool end;
		u8 *entry = __exfat_dentry_next(&dctx, 0, 0, false, &end);
		u8 type;

		if (!entry && end)
			/* genuine end of file */
			break;
		if (!entry) {
			/* something went wrong */
			error = -EIO;
			goto out;
		}
		type = *entry;

		if (put_user(type, &uhead->entries[i])) {
			error = -EFAULT;
			goto out;
		}
	}

	/*
	 * update head nr_entries and offset.
	 */
	if (put_user(i, &uhead->nr_entries))  {
		error = -EFAULT;
		goto out;
	}
	if (put_user(head.offset + 0x20 * i, &uhead->offset)) {
		error = -EFAULT;
		goto out;
	}

 out:
	exfat_cleanup_dir_ctx(&dctx);
	return error;
}

long exfat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case EXFAT_IOCGETFRAGMENTS:
		return exfat_ioctl_get_fragments(file_inode(file),
						 (void __user*)arg);
	case EXFAT_IOCGETBITMAP:
		return exfat_ioctl_get_bitmap(file_inode(file)->i_sb,
					      (void __user*)arg);
	case EXFAT_IOCGETDIRENTS:
		return exfat_ioctl_get_dirents(file_inode(file),
					       (void __user*)arg);
	default:
		return -ENOTTY;
	}
}

static int exfat_cont_expand(struct inode *inode, loff_t newsize)
{
	int error;

	error = generic_cont_expand_simple(inode, newsize);
	if (error)
		return error;

	inode->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);

	if (IS_SYNC(inode))
		exfat_msg(inode->i_sb, KERN_ERR, "TODO: cont_expand with "
			  "sync mode.");
	return 0;
}

int exfat_truncate_blocks(struct inode *inode, loff_t newsize)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	u32 fcluster = (newsize + sbi->clustersize - 1) >> sbi->clusterbits;
	int error;

	if (EXFAT_I(inode)->mmu_private > newsize)
		EXFAT_I(inode)->mmu_private = newsize;

	error = exfat_free_clusters_inode(inode, fcluster);
	if (error) {
		exfat_msg(inode->i_sb, KERN_INFO, "exfat_free_clusters_inode: "
			  "%i", error);
		return error;
	}

	return 0;
}

int exfat_getattr(struct vfsmount *mnt, struct dentry *dentry,
		  struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	generic_fillattr(inode, stat);
	stat->blksize = EXFAT_SB(inode->i_sb)->clustersize;
	return 0;
}

#define EXFAT_VALID_MODE       (S_IFREG | S_IFDIR | S_IRWXUGO)

static int exfat_mode_fixup(struct inode *inode, mode_t *mode)
{
	mode_t mask, perm;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);

	if (S_ISDIR(*mode))
		mask = sbi->options.dmask;
	else
		mask = sbi->options.fmask;

	perm = *mode & ~(S_IFMT | mask);

	/*
	 * we want 'r' and 'x' bits when mask allows for it.
	 */
	if ((perm & (S_IRUGO | S_IXUGO)) !=
	    (inode->i_mode & ~mask & (S_IRUGO | S_IXUGO))) {
		return -EPERM;
	}

	/*
	 * we want all 'w' bits or none, depending on mask.
	 */
	if ((perm & S_IWUGO) && (perm & S_IWUGO) != (~mask & S_IWUGO))
		return -EPERM;
	*mode &= ~mask;
	return 0;
}

int exfat_setattr(struct dentry *dentry, struct iattr *attrs)
{
	struct inode *inode = dentry->d_inode;
	int error;

	/*
	 * can set uid/gid, only if it the same as the current one in
	 * the inode.
	 */
	if (attrs->ia_valid & ATTR_UID &&
	    !uid_eq(inode->i_uid, attrs->ia_uid))
		return -EPERM;

	if (attrs->ia_valid & ATTR_GID &&
	    !gid_eq(inode->i_gid, attrs->ia_gid))
		return -EPERM;

	if (attrs->ia_valid & ATTR_MODE &&
	    (attrs->ia_mode & ~EXFAT_VALID_MODE ||
	     exfat_mode_fixup(inode, &attrs->ia_mode) < 0)) {
		/*
		 * silently ignore mode change if we're not OK with
		 * it (same behavior as vfat).
		 */
		attrs->ia_valid &= ~ATTR_MODE;
	}

	if (attrs->ia_valid & ATTR_SIZE) {
		inode_dio_wait(inode);
		if (attrs->ia_size > inode->i_size) {
			/*
			 * expand file
			 */
			error = exfat_cont_expand(inode, attrs->ia_size);
			if (error)
				return error;
		} else {
			/*
			 * shrink file
			 */
			truncate_setsize(inode, attrs->ia_size);
			exfat_truncate_blocks(inode, attrs->ia_size);
		}
	}

	setattr_copy(inode, attrs);
	mark_inode_dirty(inode);
	return 0;
}
