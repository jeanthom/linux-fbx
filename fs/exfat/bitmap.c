/*
 * bitmap.c for exfat
 * Created by <nschichan@freebox.fr> on Thu Aug  8 19:21:05 2013
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "exfat.h"
#include "exfat_fs.h"


static inline sector_t exfat_bitmap_sector(struct exfat_sb_info *sbi,
					   u32 cluster)
{
	return sbi->first_bitmap_sector + ((cluster / 8) >> sbi->sectorbits);
}

static inline u32 exfat_bitmap_off(struct exfat_sb_info *sbi,
				   u32 cluster)
{
	return (cluster / 8) & sbi->sectormask;
}

static inline u32 exfat_bitmap_shift(u32 cluster)
{
	return cluster & 7;
}

static int __find_get_free_cluster(struct inode *inode, u32 *out_cluster)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);

	while (1) {
		sector_t sect = exfat_bitmap_sector(sbi,
						    sbi->cur_bitmap_cluster);
		u32 off = exfat_bitmap_off(sbi, sbi->cur_bitmap_cluster);
		u32 shift = exfat_bitmap_shift(sbi->cur_bitmap_cluster);

		/* disk is full */
		if (!sbi->free_clusters)
			break;

		if (!sbi->cur_bitmap_bh ||
		    sect != sbi->cur_bitmap_sector) {
			if (sbi->cur_bitmap_bh)
				brelse(sbi->cur_bitmap_bh);
			sbi->cur_bitmap_bh = sb_bread(inode->i_sb, sect);
			sbi->cur_bitmap_sector = sect;
			if (!sbi->cur_bitmap_bh) {
				exfat_msg(inode->i_sb, KERN_ERR,
					  "unable to read bitmap sector "
					  "at %llu", sect);
				return -EIO;
			}
		}

		if (!(sbi->cur_bitmap_bh->b_data[off] & (1 << shift))) {
			sbi->cur_bitmap_bh->b_data[off] |= (1 << shift);
			*out_cluster = sbi->cur_bitmap_cluster;
			goto found;
		}

		++sbi->cur_bitmap_cluster;
		if (sbi->cur_bitmap_cluster == sbi->cluster_count)
			sbi->cur_bitmap_cluster = 0;
	}
	return -ENOSPC;

found:
	sbi->prev_free_cluster = *out_cluster;
	--sbi->free_clusters;
	mark_buffer_dirty(sbi->cur_bitmap_bh);
	return 0;
}

static int __put_cluster(struct inode *inode, u32 cluster)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	sector_t sect = exfat_bitmap_sector(sbi, cluster);
	u32 off = exfat_bitmap_off(sbi, cluster);
	u32 shift = exfat_bitmap_shift(cluster);


	if (!sbi->cur_bitmap_bh || sect != sbi->cur_bitmap_sector) {
		if (sbi->cur_bitmap_bh)
			brelse(sbi->cur_bitmap_bh);
		sbi->cur_bitmap_bh = sb_bread(inode->i_sb, sect);
		if (!sbi->cur_bitmap_bh) {
			exfat_msg(inode->i_sb, KERN_ERR,
				  "unable to read bitmap sector at %llu", sect);
			return -EIO;
		}
		sbi->cur_bitmap_sector = sect;
		sbi->cur_bitmap_cluster = cluster;
	}
	if ((sbi->cur_bitmap_bh->b_data[off] & (1 << shift)) == 0) {
		exfat_fs_error(inode->i_sb, "put_cluster: cluster %u "
			  "already free.", cluster);
		return -EIO;
	}

	++sbi->free_clusters;
	sbi->cur_bitmap_bh->b_data[off] &= ~(1 << shift);
	sbi->prev_free_cluster = cluster;
	mark_buffer_dirty(sbi->cur_bitmap_bh);
	/* sync_dirty_buffer(sbi->cur_bitmap_bh); */
	return 0;
}

/*
 * setup search to start at given cluster.
 */
static void __exfat_reset_bitmap(struct exfat_sb_info *sbi, u32 cluster)
{
	sector_t sect;

	if (cluster >= sbi->cluster_count)
		cluster = 0;

	sect = exfat_bitmap_sector(sbi, cluster);
	if (sbi->cur_bitmap_sector != sect) {
		sbi->cur_bitmap_sector = sect;
		if (sbi->cur_bitmap_bh) {
			brelse(sbi->cur_bitmap_bh);
			sbi->cur_bitmap_bh = NULL;
		}
	}
	sbi->cur_bitmap_cluster = cluster;
}

static bool all_contiguous(u32 *clusters, u32 nr)
{
	u32 i;

	for (i = 0; i < nr - 1; ++i) {
		if (clusters[i] != clusters[i + 1] - 1)
			return false;
	}
	return true;
}

/*
 * hint must be the immediately after the last allocated cluster of
 * the inode.
 */
int exfat_alloc_clusters(struct inode *inode, u32 hint, u32 *clusters, u32 nr)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *info = EXFAT_I(inode);
	u32 i;

	mutex_lock(&sbi->bitmap_mutex);
	__exfat_reset_bitmap(sbi, hint - 2);
	for (i = 0; i < nr; ++i) {
		u32 new;
		int error;

		error = __find_get_free_cluster(inode, &new);
		if (error) {
			mutex_unlock(&sbi->bitmap_mutex);
			return error;
		}

		clusters[i] = new + 2;
	}
	mutex_unlock(&sbi->bitmap_mutex);

	/*
	 * all clusters found: now see if we need to update/create a
	 * fat chain.
	 */
	if (info->first_cluster == 0) {
		info->first_cluster = clusters[0];
		if (all_contiguous(clusters, nr)) {
			/*
			 * first cluster alloc on inode and all
			 * clusters are contiguous.
			 */
			info->flags |= EXFAT_I_FAT_INVALID;
		} else {
			/*
			 * first alloc and already fragmented.
			 */
			return exfat_write_fat(inode, 0, clusters, nr);
		}
	} else {
		int error;
		if ((info->flags & EXFAT_I_FAT_INVALID) &&
		    (clusters[0] != hint || !all_contiguous(clusters, nr))) {
			/*
			 * must now use fat chain instead of bitmap.
			 */
			info->flags &= ~(EXFAT_I_FAT_INVALID);

			/*
			 * write the contiguous chain that would
			 * previously be accessed without the FAT
			 * chain.
			 */
			error = exfat_write_fat_contiguous(inode,
						  info->first_cluster,
						  hint - info->first_cluster);
			if (error)
				return error;
		}

		if ((info->flags & EXFAT_I_FAT_INVALID) == 0) {
			/*
			 * link the allocated clusters after hint.
			 */
			error = exfat_write_fat(inode, hint - 1, clusters, nr);
			if (error)
				return  error;
		}

	}

	/*
	 * update i_blocks.
	 */
	inode->i_blocks += nr << (sbi->clusterbits - 9);
	info->allocated_clusters += nr;

	/*
	 * caller must call mark_inode_dirty so that inode
	 * first_cluster and inode flags get written to the disk.
	 * caller must update inode size (directory and regular file
	 * have different rules).
	 */
	return 0;
}


static int exfat_free_clusters_contiguous(struct inode *inode,
					  u32 start, u32 nr)
{
	u32 cluster;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	int error = 0;

	mutex_lock(&sbi->bitmap_mutex);
	for (cluster = start; cluster < start + nr; ++cluster) {
		error = __put_cluster(inode, cluster - 2);
		if (error)
			break;
	}
	mutex_unlock(&sbi->bitmap_mutex);
	return error;
}

static int exfat_free_clusters_fat(struct inode *inode,
				   u32 fcluster_start, u32 nr)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	u32 fcluster;
	int error = 0;

	mutex_lock(&sbi->bitmap_mutex);
	for (fcluster = fcluster_start; fcluster < fcluster_start + nr;
	     ++fcluster) {
		u32 dcluster;
		int error;

		error = exfat_get_fat_cluster(inode, fcluster, &dcluster);
		if (error)
			break;

		error = __put_cluster(inode, dcluster - 2);
		if (error)
			break;
	}
	mutex_unlock(&sbi->bitmap_mutex);

	/*
	 * per-inode file cluster to disk cluster translation cache
	 * mostly now holds entries to the zone we just truncated, so
	 * they must not be kept (this could lead to FS corruption).
	 */
	exfat_inode_cache_drop(inode);

	return error;
}

int exfat_free_clusters_inode(struct inode *inode, u32 fcluster_start)
{
	struct exfat_inode_info *info = EXFAT_I(inode);
	int error;
	u32 nr_to_free = info->allocated_clusters - fcluster_start;

	if (info->first_cluster == 0 || nr_to_free == 0)
		/*
		 * no clusters allocated, or nothing to do
		 */
		return 0;

	if (info->flags & EXFAT_I_FAT_INVALID)
		error = exfat_free_clusters_contiguous(inode,
				       info->first_cluster + fcluster_start,
				       nr_to_free);
	else
		error = exfat_free_clusters_fat(inode, fcluster_start,
					nr_to_free);
	if (error)
		return error;

	info->allocated_clusters -= nr_to_free;
	inode->i_blocks = EXFAT_I(inode)->allocated_clusters <<
		(EXFAT_SB(inode->i_sb)->clusterbits - 9);

	/*
	 * update inode info, caller must call mark_inode_dirty and
	 * update inode->i_size.
	 */
	if (fcluster_start == 0) {
		info->first_cluster = 0;
		info->flags &= ~(EXFAT_I_FAT_INVALID);
	}
	return 0;
}

static u32 count_clusters_bh(struct buffer_head *bh, u32 count)
{
	u8 *ptr = bh->b_data;
	u32 ret = 0;
	u8 val;

	while (count >= sizeof (u64) * 8) {
		u64 val = *(u64*)ptr;

		ret += hweight64(~val);
		count -= sizeof (u64) * 8;
		ptr += sizeof (u64);
	}
	if (count >= sizeof (u32) * 8) {
		u32 val = *(u32*)ptr;

		ret += hweight32(~val);
		count -= sizeof (u32) * 8;
		ptr += sizeof (u32);
	}
	if (count >= sizeof (u16) * 8) {
		u16 val = *(u16*)ptr;

		ret += hweight16(~val);
		count -= sizeof (u16) * 8;
		ptr += sizeof (u16);
	}
	while (count >= sizeof (u8) * 8) {
		u8 val = *ptr;

		ret += hweight8(~val);
		count -= sizeof (u8) * 8;
		ptr += sizeof (u8);
	}
	val = *ptr;
	while (count) {
		ret += (~val & 1);
		val >>= 1;
		--count;
	}
	return ret;
}

/*
 * only called during mount, so taking sbi->bitmap_mutex should not be
 * needed.
 */
static int exfat_get_free_cluster_count(struct super_block *sb, u32 *out_count)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	u32 clusters_per_sector = 8 * sbi->sectorsize;
	u32 cluster;

	*out_count = 0;
	for (cluster = 0; cluster < sbi->cluster_count;
	     cluster += clusters_per_sector) {
		sector_t sect = exfat_bitmap_sector(sbi, cluster);
		struct buffer_head *bh;
		u32 count = clusters_per_sector;

		if (cluster + clusters_per_sector > sbi->cluster_count)
			count = sbi->cluster_count - cluster;

		bh = sb_bread(sb, sect);
		if (!bh) {
			exfat_msg(sb, KERN_ERR,
				  "unable to read bitmap sector at %llu", sect);
			return -EIO;
		}
		*out_count += count_clusters_bh(bh, count);
		brelse(bh);
	}
	return 0;
}

/*
 * setup a bitmap context, preload a bh from the requested starting
 * cluster.
 */
int exfat_init_bitmap_context(struct super_block *sb,
			      struct exfat_bitmap_ctx *ctx,
			      u32 cluster)
{
	memset(ctx, 0, sizeof (*ctx));
	ctx->sb = sb;

	cluster -= 2;
	if (cluster >= EXFAT_SB(sb)->cluster_count)
		return -ENOSPC;

	ctx->cur_sector = exfat_bitmap_sector(EXFAT_SB(sb), cluster);
	ctx->bh = sb_bread(ctx->sb, ctx->cur_sector);

	if (!ctx->bh) {
		exfat_msg(sb, KERN_ERR, "unable to read bitmap sector at %llu",
			  ctx->cur_sector);
		return -EIO;
	}
	return 0;
}

/*
 * release bh in an already setup bitmap context.
 */
void exfat_exit_bitmap_context(struct exfat_bitmap_ctx *ctx)
{
	if (ctx->bh)
		brelse(ctx->bh);
}

/*
 * test a specific cluster usage in the bitmap. reuse the bh in the
 * exfat_bitmap_ctx or read a new one if starting cluster is outside
 * the current one.
 */
static int exfat_test_bitmap_cluster(struct exfat_bitmap_ctx *ctx,
				     uint32_t cluster, bool *cluster_in_use)
{
	sector_t sect;
	uint32_t off = exfat_bitmap_off(EXFAT_SB(ctx->sb), cluster);
	int shift = exfat_bitmap_shift(cluster);

	sect = exfat_bitmap_sector(EXFAT_SB(ctx->sb), cluster);
	if (sect != ctx->cur_sector) {
		ctx->cur_sector = sect;
		ctx->bh = sb_bread(ctx->sb, ctx->cur_sector);
		if (!ctx->bh) {
			exfat_msg(ctx->sb, KERN_ERR,
				  "unable to read bitmap sector at %llu", sect);
			return -EIO;
		}
	}

	*cluster_in_use = !!(ctx->bh->b_data[off] & (1 << shift));
	return 0;
}

/*
 * update first_in_use and nr_in_use with the first zone of used
 * clusters starting from start_cluster.
 */
int exfat_test_bitmap(struct exfat_bitmap_ctx *ctx, uint32_t start_cluster,
		      uint32_t *first_in_use, uint32_t *nr_in_use)
{
	bool in_use = false;
	int error = 0;
	struct exfat_sb_info *sbi = EXFAT_SB(ctx->sb);

	start_cluster -= 2;

	/*
	 * scan bitmap until we find a cluster that is in use.
	 */
	while (1) {
		if (start_cluster == sbi->cluster_count) {
			/*
			 * readched end of disk: no more in use
			 * cluster found.
			 */
			*first_in_use = sbi->cluster_count;
			*nr_in_use = 0;
			return 0;
		}
		error = exfat_test_bitmap_cluster(ctx, start_cluster, &in_use);
		if (error)
			return error;
		if (in_use)
			break;
		++start_cluster;
	}


	/*
	 * update first_in_use, and scan until a free cluster is
	 * found.
	 */
	*first_in_use = start_cluster + 2;
	*nr_in_use = 0;
	while (1) {
		error = exfat_test_bitmap_cluster(ctx, start_cluster, &in_use);
		if (error)
			return error;
		if (!in_use)
			break;
		++(*nr_in_use);
		++start_cluster;
	}
	return 0;
}

int exfat_init_bitmap(struct inode *root)
{
	struct exfat_sb_info *sbi = EXFAT_SB(root->i_sb);
	struct exfat_bitmap_entry *be;
	struct exfat_dir_ctx dctx;
	u32 first_bitmap_cluster;
	u32 last_bitmap_cluster;

	int error;

	mutex_init(&sbi->bitmap_mutex);

	error = exfat_init_dir_ctx(root, &dctx, 0);
	if (error)
		return error;

try_bitmap:
	error = -ENOENT;
	be = __exfat_dentry_next(&dctx, E_EXFAT_BITMAP, 0xff, true, NULL);
	if (!be) {
		exfat_msg(root->i_sb, KERN_ERR, "root directory does not "
			  "have a bitmap entry.");
		goto fail;
	}

	if (exfat_bitmap_nr(be->flags) != 0)
		/*
		 * not expected to find a second bitmap entry here
		 * since we checked during superblock fill that we
		 * were not on a texFAT volume ...
		 */
		goto try_bitmap;


	error = -EINVAL;
	if (__le64_to_cpu(be->length) * 8 < sbi->cluster_count) {
		exfat_msg(root->i_sb, KERN_INFO, "bitmap does not cover "
			  "the whole cluster heap.");
		goto fail;
	}

	first_bitmap_cluster = __le32_to_cpu(be->cluster_addr);
	last_bitmap_cluster = first_bitmap_cluster +
		(__le32_to_cpu(be->length) >> sbi->clusterbits);

	/*
	 * check that bitmap start and end clusters are inside the
	 * disk.
	 */
	error = -ERANGE;
	if (first_bitmap_cluster < 2 &&
	    first_bitmap_cluster >= sbi->cluster_count) {
		exfat_msg(root->i_sb, KERN_ERR, "bitmap start cluster is "
			  "outside disk limits.");
		goto fail;
	}
	if (last_bitmap_cluster < 2 &&
	    last_bitmap_cluster >= sbi->cluster_count) {
		exfat_msg(root->i_sb, KERN_ERR, "bitmap last cluster is "
			  "outside disk limits.");
		goto fail;
	}

	sbi->bitmap_length = __le32_to_cpu(be->length);
	sbi->first_bitmap_sector = exfat_cluster_sector(sbi,
					__le32_to_cpu(be->cluster_addr));
	sbi->last_bitmap_sector = sbi->first_bitmap_sector +
		DIV_ROUND_UP(sbi->bitmap_length, sbi->sectorsize);

	error = exfat_get_free_cluster_count(root->i_sb, &sbi->free_clusters);
	if (error)
		goto fail;

	sbi->prev_free_cluster = 0;

	exfat_cleanup_dir_ctx(&dctx);
	return 0;
fail:
	exfat_cleanup_dir_ctx(&dctx);
	return error;
}

void exfat_exit_bitmap(struct super_block *sb)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);

	if (sbi->cur_bitmap_bh)
		brelse(sbi->cur_bitmap_bh);
}
