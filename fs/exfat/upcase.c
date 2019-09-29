/*
 * upcase.c for exfat
 * Created by <nschichan@freebox.fr> on Wed Aug  7 11:51:37 2013
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/fs.h>

#include "exfat.h"
#include "exfat_fs.h"

static u32 exfat_calc_upcase_checksum(const u8 *data, u32 checksum,
				      size_t count)
{
	while (count) {
		checksum = ((checksum << 31) | (checksum >> 1)) + *data;
		--count;
		++data;
	}
	return checksum;
}

static int exfat_load_upcase_table(struct super_block *sb, u32 disk_cluster,
				   u32 *out_checksum)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct buffer_head *bh;
	sector_t start, sect, end;
	u32 off = 0;
	u32 byte_len = sbi->upcase_len * sizeof (__le16);
	u32 checksum = 0;

	/*
	 * up-case table are not fragmented, so sequential cluster
	 * read will do here.
	 */
	start = exfat_cluster_sector(sbi, disk_cluster);
	end = start + DIV_ROUND_UP(byte_len,
			   sbi->sectorsize);
	for (sect = start; sect < end; ++sect) {
		u32 len = sbi->sectorsize;

		if (sect == end - 1)
			len = byte_len & sbi->sectormask;

		bh = sb_bread(sb, sect);
		if (!bh) {
			exfat_msg(sb, KERN_ERR,
				  "unable to read upcase sector %llu", sect);
			return -EIO;
		}
		memcpy((u8*)sbi->upcase_table + off, bh->b_data,
		       len);

		checksum = exfat_calc_upcase_checksum(bh->b_data, checksum,
						      len);

		off += len;
		brelse(bh);
	}

	BUG_ON(off != byte_len);
	*out_checksum = checksum;
	return 0;
}

int exfat_upcase_init(struct inode *root)
{
	struct exfat_sb_info *sbi = EXFAT_SB(root->i_sb);
	struct exfat_upcase_entry *upcase;
	struct exfat_dir_ctx dctx;
	int error;
	u64 upcase_length;
	u32 checksum;

	/*
	 * configure directory context and look for an upcase table
	 * entry.
	 */
	if (exfat_init_dir_ctx(root, &dctx, 0) < 0)
		return -EIO;

	error = -EIO;
	upcase = __exfat_dentry_next(&dctx, E_EXFAT_UPCASE_TABLE, 0xff,
				     true, NULL);
	if (!upcase)
		goto fail;

	/*
	 * check upcase table length. we need it to be non-zero,
	 * ending on a __le16 boundary and provide at most a
	 * conversion for the whole __le16 space.
	 */
	upcase_length = __le64_to_cpu(upcase->length);
	if (upcase_length == 0 ||
	    upcase_length & (sizeof (__le16) - 1) ||
	    upcase_length > 0xffff * sizeof (__le16)) {
		exfat_msg(root->i_sb, KERN_ERR, "invalid upcase length %llu",
			  upcase_length);
		goto fail;
	}

	/*
	 * load complete upcase table in memory.
	 */
	error = -ENOMEM;
	sbi->upcase_len = upcase_length / sizeof (__le16);
	sbi->upcase_table = kmalloc(upcase_length, GFP_NOFS);
	if (!sbi->upcase_table)
		goto fail;

	error = exfat_load_upcase_table(root->i_sb,
					__le32_to_cpu(upcase->cluster_addr),
					&checksum);
	if (error)
		goto fail;

	if (checksum != __le32_to_cpu(upcase->checksum)) {
		exfat_msg(root->i_sb, KERN_INFO,
			  "upcase table checksum mismatch: have %08x, "
			  "expect %08x", checksum,
			  __le32_to_cpu(upcase->checksum));
		error = -EINVAL;
		goto fail;
	}

	exfat_cleanup_dir_ctx(&dctx);
	return 0;

fail:
	if (sbi->upcase_table)
		kfree(sbi->upcase_table);
	exfat_cleanup_dir_ctx(&dctx);
	return error;
}
