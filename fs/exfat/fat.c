/*
 * fat.c for exfat
 * Created by <nschichan@freebox.fr> on Mon Jul 29 19:43:38 2013
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>

#include "exfat.h"
#include "exfat_fs.h"

#define MAX_CACHED_FAT	16

/*
 * helpers for exfat_next_fat_cluster.
 */

/*
 * get the sector number in the fat where the next requested cluster
 * number is to be found.
 */
static inline sector_t cluster_sector(struct exfat_sb_info *sbi, u32 cluster)
{
	return sbi->fat_offset + (((u64)cluster * sizeof (u32)) >> sbi->sectorbits);
}

/*
 * get the offset in the fat sector where the next requested cluster
 * number is to be found.
 */
static inline off_t cluster_offset(struct exfat_sb_info *sbi, u32 cluster)
{
	return (cluster * sizeof (u32)) & sbi->sectormask;
}

/*
 * walk one step in the fat chain.
 */
static int exfat_next_fat_cluster(struct super_block *sb, u32 *cluster)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	sector_t sect = cluster_sector(sbi, *cluster);
	off_t off = cluster_offset(sbi, *cluster);
	struct buffer_head *bh;

	bh = sb_bread(sb, sect);
	if (!bh) {
		exfat_msg(sb, KERN_ERR, "unable to read FAT sector at %llu",
			  sect);
		return -EIO;
	}

	*cluster = __le32_to_cpu(*(u32*)&bh->b_data[off]);
	brelse(bh);
	return 0;
}

/*
 * setup inode cache
 */
void exfat_inode_cache_init(struct inode *inode)
{
	mutex_init(&EXFAT_I(inode)->exfat_cache.mutex);
	EXFAT_I(inode)->exfat_cache.nr_entries = 0;
	INIT_LIST_HEAD(&EXFAT_I(inode)->exfat_cache.entries);
}

/*
 * drop inode cache content
 */
void exfat_inode_cache_drop(struct inode *inode)
{
	struct exfat_cache *cache = &EXFAT_I(inode)->exfat_cache;
	struct exfat_cache_entry *e, *tmp;

	mutex_lock(&cache->mutex);
	list_for_each_entry_safe (e, tmp, &cache->entries, list) {
		kfree(e);
	}
	INIT_LIST_HEAD(&cache->entries);
	cache->nr_entries = 0;
	mutex_unlock(&cache->mutex);
}

/*
 * move the entry to the head of the list, this will make it less
 * likely to be the victim in when caching new entries.
 *
 * caller must hold cache->mutex.
 */
static void __exfat_fat_lru(struct exfat_cache *cache,
			  struct exfat_cache_entry *e)
{
	if (cache->entries.next != &e->list)
		list_move(&e->list, &cache->entries);
}

/*
 * find a cache entry that is close to the wanted fcluster (ideally
 * spanning over the requested file cluster).
 *
 * caller must hold cache->mutex.
 */
static struct exfat_cache_entry *__exfat_cache_lookup(struct exfat_cache *cache,
						      u32 fcluster)
{
	struct exfat_cache_entry *e;
	struct exfat_cache_entry *best = NULL;

	list_for_each_entry (e, &cache->entries, list) {
		if (e->file_cluster <= fcluster &&
		    e->file_cluster + e->nr_contig >= fcluster)
			return e;

		if (!best && e->file_cluster < fcluster)
			best = e;
		if (best && best->file_cluster < e->file_cluster &&
		    e->file_cluster < fcluster)
			best = e;
	}
	return best;
}

/*
 * caller must hold cache->mutex.
 */
static int __exfat_cache_cluster(struct exfat_cache *cache,
			       struct exfat_cache_entry *nearest,
			       u32 fcluster, u32 dcluster)
{
	struct exfat_cache_entry *e;

	/*
	 * see if we can merge with the nearest entry. in the ideal
	 * case, all cluster in the chain are contiguous, and only
	 * one entry is needed for a single file.
	 */
	if (nearest &&
	    nearest->file_cluster + nearest->nr_contig + 1 == fcluster &&
	    nearest->disk_cluster + nearest->nr_contig + 1 == dcluster) {
		list_move(&nearest->list, &cache->entries);
		nearest->nr_contig++;
		return 0;
	}

	/*
	 * allocate a new entry or reuse an existing one if the number
	 * of cached entries is too hihc.
	 */
	if (cache->nr_entries < MAX_CACHED_FAT) {
		e = kmalloc(sizeof (*e), GFP_NOFS);
		list_add(&e->list, &cache->entries);
		++cache->nr_entries;
	} else {
		e = list_entry(cache->entries.prev, struct exfat_cache_entry,
			       list);
		list_move(&e->list, &cache->entries);
	}

	if (!e)
		return -ENOMEM;

	e->file_cluster = fcluster;
	e->disk_cluster = dcluster;
	e->nr_contig = 0;

	return 0;
}

int __exfat_get_fat_cluster(struct inode *inode, u32 fcluster, u32 *dcluster,
			    bool eof_is_fatal)
{
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct exfat_cache *cache = &info->exfat_cache;
	int error;
	struct exfat_cache_entry *e;
	u32 fcluster_start;

	/*
	 * intial translation: first file cluster is found in the
	 * inode info.
	 */
	if (fcluster == 0) {
		*dcluster = info->first_cluster;
		return 0;
	}

	mutex_lock(&cache->mutex);
	/*
	 * try to find a cached entry either covering the file cluster
	 * we want or at least close to the file cluster.
	 */
	e = __exfat_cache_lookup(cache, fcluster);
	if (e && e->file_cluster <= fcluster &&
	    e->file_cluster + e->nr_contig >= fcluster) {
		/*
		 * perfect match, entry zone covers the requested file
		 * cluster.
		 */
		__exfat_fat_lru(cache, e);
		*dcluster = e->disk_cluster + (fcluster - e->file_cluster);
		mutex_unlock(&cache->mutex);
		return 0;
	}

	if (e) {
		/*
		 * we have an entry, hopefully close enough, setup
		 * cluster walk from there.
		 */
		*dcluster = e->disk_cluster + e->nr_contig;
		fcluster_start = e->file_cluster + e->nr_contig;
	} else {
		/*
		 * no entry, walk the FAT chain from the start of the
		 * file.
		 */
		fcluster_start = 0;
		*dcluster = info->first_cluster;
	}

	/*
	 * walk fhe FAT chain the number of time required to get the
	 * disk cluster corresponding to the file cluster.
	 */
	while (fcluster_start != fcluster) {
		error = exfat_next_fat_cluster(inode->i_sb, dcluster);
		if (error) {
			mutex_unlock(&cache->mutex);
			return error;
		}
		if (*dcluster == EXFAT_CLUSTER_EOF) {
			if (eof_is_fatal)
				/*
				 * exfat_fill_root uses
				 * __exfat_get_fat_cluster with
				 * eof_is_fatal set to false, as the
				 * root inode does not have a size
				 * field and thus requires a complete
				 * FAT walk to compute the size.
				 */
				exfat_fs_error(inode->i_sb, "premature EOF in FAT "
					       "chain. file cluster %u out "
					       "of %u\n", fcluster_start,
					       fcluster);
			mutex_unlock(&cache->mutex);
			return -EIO;
		}
		if (*dcluster < EXFAT_CLUSTER_FIRSTVALID) {
			exfat_fs_error(inode->i_sb, "invalid cluster %u found "
				       "in fat chain.", *dcluster);
			mutex_unlock(&cache->mutex);
			return -EIO;
		}
		++fcluster_start;
	}

	/*
	 * cache the result.
	 */
	__exfat_cache_cluster(cache, e, fcluster, *dcluster);
	mutex_unlock(&cache->mutex);
	return 0;
}

int exfat_get_fat_cluster(struct inode *inode, u32 fcluster, u32 *dcluster)
{
	return __exfat_get_fat_cluster(inode, fcluster, dcluster, true);
}

int exfat_init_fat(struct super_block *sb)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct buffer_head *bh;
	int error = 0;
	u32 first, second;

	bh = sb_bread(sb, sbi->fat_offset);
	if (!bh) {
		exfat_msg(sb, KERN_ERR, "unable to read FAT sector at %u",
			  sbi->fat_offset);
		return -EIO;
	}

	first = __le32_to_cpu(*(__le32*)(bh->b_data + 0));
	second = __le32_to_cpu(*(__le32*)(bh->b_data + sizeof (__le32)));

	if (first != 0xf8ffffff && second != 0xffffffff) {
		exfat_msg(sb, KERN_INFO, "invalid FAT start: %08x, %08x",
			  first, second);
		error = -ENXIO;
	}

	brelse(bh);
	return error;
}

/*
 * fat write context, store the current buffer_head and current
 * cluster to avoid having sb_bread all the time when the clusters are
 * contiguous or at least not too far apart.
 */
struct fat_write_ctx {
	struct super_block *sb;
	struct buffer_head *bh;
	u32 cur_cluster;
};

static void fat_init_write_ctx(struct fat_write_ctx *fwctx,
				struct super_block *sb)
{
	memset(fwctx, 0, sizeof (*fwctx));
	fwctx->sb = sb;
}

static void fat_exit_write_ctx(struct fat_write_ctx *fwctx)
{
	if (fwctx->bh)
		brelse(fwctx->bh);
}

static int __fat_write_entry(struct fat_write_ctx *fwctx,
			       u32 cluster, u32 next)
{
	struct exfat_sb_info *sbi = EXFAT_SB(fwctx->sb);
	sector_t current_sector = cluster_sector(sbi, fwctx->cur_cluster);
	sector_t wanted_sector = cluster_sector(sbi, cluster);
	off_t off = cluster_offset(sbi, cluster);

	/*
	 * first see if we need a different buffer head from the
	 * current one in the fat_write_ctx.
	 */
	if (current_sector != wanted_sector || !fwctx->bh) {
		if (fwctx->bh)
			brelse(fwctx->bh);
		fwctx->bh = sb_bread(fwctx->sb, wanted_sector);
		if (!fwctx->bh) {
			exfat_msg(fwctx->sb, KERN_ERR,
				  "unable to read FAT sector at %llu",
				  wanted_sector);
			return -EIO;
		}
	}

	/*
	 * set fat cluster to point to the next cluster, and mark bh
	 * dirty so that the change hits the storage device.
	 */
	fwctx->cur_cluster = cluster;
	*(__le32*)(fwctx->bh->b_data + off) = __cpu_to_le32(next);
	mark_buffer_dirty(fwctx->bh);
	return 0;
}

/*
 * write nr_clusters contiguous clusters starting at first_cluster.
 */
int exfat_write_fat_contiguous(struct inode *inode, u32 first_cluster,
			       u32 nr_clusters)
{
	u32 cluster;
	struct fat_write_ctx fwctx;
	int error = 0;

	fat_init_write_ctx(&fwctx, inode->i_sb);
	for (cluster = first_cluster;
	     cluster < first_cluster + nr_clusters - 1;
	     ++cluster) {
		error = __fat_write_entry(&fwctx, cluster, cluster + 1);
		if (error)
			goto end;
	}

	/*
	 * set EOF
	 */
	error = __fat_write_entry(&fwctx, cluster, EXFAT_CLUSTER_EOF);
end:
	fat_exit_write_ctx(&fwctx);
	return error;

}

/*
 * write cluster nr_clusters stored in clusters array, link with prev_cluster.
 */
int exfat_write_fat(struct inode *inode, u32 prev_cluster, u32 *clusters,
		    u32 nr_clusters)
{
	u32 i;
	struct fat_write_ctx fwctx;
	int error;

	if (!nr_clusters)
		/* ??! */
		return 0;

	fat_init_write_ctx(&fwctx, inode->i_sb);

	if (prev_cluster) {
		/*
		 * link with previous cluster if applicable.
		 */
		error = __fat_write_entry(&fwctx, prev_cluster, clusters[0]);
		if (error)
			goto end;
	}
	for (i = 0; i < nr_clusters - 1; ++i) {
		error = __fat_write_entry(&fwctx, clusters[i], clusters[i + 1]);
		if (error)
			goto end;
	}

	/*
	 * set EOF.
	 */
	error = __fat_write_entry(&fwctx, clusters[i], EXFAT_CLUSTER_EOF);

 end:
	fat_exit_write_ctx(&fwctx);
	return error;
}
