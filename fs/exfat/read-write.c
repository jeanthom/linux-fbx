/*
 * read-write.c for exfat
 * Created by <nschichan@freebox.fr> on Wed Jul 31 16:37:51 2013
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/buffer_head.h>

#include "exfat.h"
#include "exfat_fs.h"

/*
 * map file sector to disk sector.
 */
static int exfat_bmap(struct inode *inode, sector_t fsect, sector_t *dsect)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *info = EXFAT_I(inode);
	u32 cluster_nr = fsect >> (sbi->clusterbits - sbi->sectorbits);
	u32 cluster;
	unsigned int offset = fsect & (sbi->sectors_per_cluster - 1);

	if (info->flags & EXFAT_I_FAT_INVALID)
		cluster = info->first_cluster + cluster_nr;
	else {
		int error;

		error = exfat_get_fat_cluster(inode, cluster_nr, &cluster);
		if (error)
			return error;
	}

	*dsect = exfat_cluster_sector(sbi, cluster) + offset;
	return 0;
}

static int exfat_get_block(struct inode *inode, sector_t block,
			   struct buffer_head *bh, int create)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *info = EXFAT_I(inode);
	sector_t last_block;
	unsigned int offset;
	sector_t dblock;
	int error;

	last_block = (i_size_read(inode) + sbi->sectorsize - 1) >>
		sbi->sectorbits;
	offset = block & (sbi->sectors_per_cluster - 1);

	if (!create && block >= last_block)
		return 0;

	if (create && block >= last_block && offset == 0) {
		u32 hint, cluster;

		/*
		 * request for first sector in a cluster immediate to
		 * the last allocated cluster of the file: must
		 * allocate a new clluster.
		 */
		error = exfat_get_cluster_hint(inode, &hint);
		if (error)
			return error;

		error = exfat_alloc_clusters(inode, hint, &cluster, 1);
		if (error)
			return error;
	}

	error = exfat_bmap(inode, block, &dblock);
	if (error)
		return error;

	if (create && block >= last_block) {
		/*
		 * currently in create mode: we need to update
		 * mmu_private.
		 */
		info->mmu_private += sbi->sectorsize;
		set_buffer_new(bh);
	}
	map_bh(bh, inode->i_sb, dblock);
	return 0;
}

int exfat_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, exfat_get_block);
}

int exfat_readpages(struct file *file, struct address_space *mapping,
		    struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, exfat_get_block);
}

static int exfat_write_error(struct inode *inode, loff_t to)
{
	if (to > inode->i_size) {
		truncate_pagecache(inode, to);
		exfat_truncate_blocks(inode, inode->i_size);
	}
	return 0;
}

int exfat_write_begin(struct file *file, struct address_space *mapping,
		      loff_t pos, unsigned len, unsigned flags,
		      struct page **pagep, void **fsdata)
{
	struct inode *inode = mapping->host;
	int error;

	*pagep = NULL;
	error = cont_write_begin(file, mapping, pos, len, flags, pagep, fsdata,
				 exfat_get_block, &EXFAT_I(inode)->mmu_private);

	if (error)
		exfat_write_error(inode, pos + len);
	return error;
}

int exfat_write_end(struct file *file, struct address_space *mapping,
		    loff_t pos, unsigned len, unsigned copied,
		    struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	int error;

	error = generic_write_end(file, mapping, pos, len, copied, page,
				  fsdata);

	if (error < len)
		exfat_write_error(inode, pos + len);
	return error;
}

int exfat_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, exfat_get_block, wbc);
}

int exfat_writepages(struct address_space *mapping,
		     struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, exfat_get_block);
}
