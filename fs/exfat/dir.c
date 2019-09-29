/*
 * dir.c for exfat
 * Created by <nschichan@freebox.fr> on Tue Aug 20 11:42:46 2013
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/nls.h>

#include "exfat.h"
#include "exfat_fs.h"

/*
 * setup an exfat_dir_ctx structure so that __exfat_dentry_next can
 * work with it.
 */
int exfat_init_dir_ctx(struct inode *inode, struct exfat_dir_ctx *ctx,
		       off_t start)
{
	u32 cluster = EXFAT_I(inode)->first_cluster;

	memset(ctx, 0, sizeof (*ctx));

	if (cluster == 0) {
		ctx->empty = true;
		ctx->sb = inode->i_sb;
		return 0;
	}

	if (cluster < EXFAT_CLUSTER_FIRSTVALID ||
	    cluster > EXFAT_CLUSTER_LASTVALID) {
		exfat_msg(inode->i_sb, KERN_ERR, "exfat_init_dir_ctx: invalid "
			  "cluster %u", cluster);
		return -EINVAL;
	}

	start &= ~(0x20 - 1);
	if (start == 0)
		ctx->off = -1;
	else
		ctx->off = start - 0x20;

	ctx->sb = inode->i_sb;
	ctx->inode = inode;

	return 0;
}

void exfat_cleanup_dir_ctx(struct exfat_dir_ctx *dctx)
{
	if (dctx->bh)
		brelse(dctx->bh);
}

/*
 * calculate the checksum for the current direntry. fields containing
 * the checksum for the first entry is not part of the checksum
 * calculation.
 */
u16 exfat_direntry_checksum(void *data, u16 checksum, bool first)
{
	u8 *ptr = data;
	int i;

	for (i = 0; i < 0x20; ++i) {
		if (first && (i == 2 || i == 3))
			continue ;
		checksum = ((checksum << 15) | (checksum >> 1)) + (u16)ptr[i];
	}
	return checksum;
}

u32 exfat_dctx_fpos(struct exfat_dir_ctx *dctx)
{
	return dctx->off;
}

u64 exfat_dctx_dpos(struct exfat_dir_ctx *dctx)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dctx->sb);

	return (dctx->sector << sbi->sectorbits) +
		(dctx->off & sbi->sectormask);
}

static int exfat_get_dctx_disk_cluster(struct exfat_dir_ctx *dctx,
				       u32 file_cluster, u32 *disk_cluster)
{
	struct exfat_inode_info *info = EXFAT_I(dctx->inode);

	if (info->flags & EXFAT_I_FAT_INVALID) {
		*disk_cluster = info->first_cluster + file_cluster;
		return 0;
	} else {
		return exfat_get_fat_cluster(dctx->inode, file_cluster,
					     disk_cluster);
	}
}

/*
 * get the next typed dentry in the exfat_dir_ctx structure. can_skip
 * indicates whether the entry must be immediately there in the entry
 * stream. *end indicates whether end of directory entry stream is
 * reached or not.
 *
 * only one buffer_head is kept at a time. subsequent calls to
 * __exfat_dentry_next can invalidate pointers from previous calls due
 * to that.
 */
void *__exfat_dentry_next(struct exfat_dir_ctx *dctx, int type, int mask,
			  bool can_skip, bool *end)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dctx->sb);

	if (dctx->empty) {
		if (end)
			*end = true;
		return NULL;
	}

	if (end)
		*end = false;

	if (dctx->off == -1)
		dctx->off = 0;
	else
		dctx->off += 0x20;

	for (;;) {
		sector_t wanted_sector;
		u32 file_cluster = dctx->off >> sbi->clusterbits;
		u32 disk_cluster;
		int error;
		int sector_offset;
		sector_t sector_in_cluster;

		if (dctx->off >= dctx->inode->i_size) {
			*end = true;
			return NULL;
		}


		error = exfat_get_dctx_disk_cluster(dctx, file_cluster,
						    &disk_cluster);
		if (error)
			return NULL;

		sector_in_cluster = (dctx->off >> sbi->sectorbits) %
			sbi->sectors_per_cluster;

		wanted_sector = exfat_cluster_sector(sbi, disk_cluster) +
			sector_in_cluster;
		if (wanted_sector != dctx->sector || !dctx->bh) {
			/*
			 * need to fetch a new sector from the current
			 * cluster.
			 */
			dctx->sector = wanted_sector;
			if (dctx->bh)
				brelse(dctx->bh);
			dctx->bh = sb_bread(dctx->sb, dctx->sector);
			if (!dctx->bh)
				return NULL;
		}

		sector_offset = dctx->off & sbi->sectormask;
		if ((dctx->bh->b_data[sector_offset] & mask) == (type & mask))
			/*
			 * return pointer to entry if type matches the
			 * one given.
			 */
			return dctx->bh->b_data + sector_offset;

		if (dctx->bh->b_data[sector_offset] == 0 && end)
			/*
			 * set end if no more entries in this directory.
			 */
			*end = true;

		if (dctx->bh->b_data[sector_offset] == 0 || !can_skip)
			/*
			 * handle can_skip / end of directory.
			 */
			return NULL;

		/*
		 * move to next entry.
		 */
		dctx->off += 0x20;
	}
	return NULL;
}

/*
 * helper around __exfat_dentry_next that copies the content of the
 * found entry in a user supplied buffer.
 */
int exfat_dentry_next(void *out, struct exfat_dir_ctx *dctx,
			     int type, bool can_skip)
{
	bool end;

	void *ptr = __exfat_dentry_next(dctx, type, 0xff, can_skip, &end);

	if (!ptr) {
		if (end)
			return -ENOENT;
		else {
			exfat_msg(dctx->sb, KERN_INFO, "no ptr and "
				  "end not reached: "
				  "type %02x, can_skip %s\n", type,
				  can_skip ? "true" : "false");
			return -EIO;
		}
	}
	memcpy(out, ptr, 0x20);
	return 0;
}

/*
 * extract name by parsing consecutive E_EXFAT_FILENAME entries in a
 * caller provided buffer. also update the checksum on the fly.
 *
 * no utf16 to utf8 conversion is performed.
 */
int __exfat_get_name(struct exfat_dir_ctx *dctx, u32 name_length,
			    __le16 *name, u16 *calc_checksum,
			    struct exfat_iloc *iloc)
{
	__le16 *ptr;
	int error;
	int nr;

	ptr = name;

	error = -EIO;
	nr = 0;
	while (name_length) {
		struct exfat_filename_entry *e;
		u32 len = 15;

		e = __exfat_dentry_next(dctx, E_EXFAT_FILENAME, 0xff,
					false, NULL);
		if (!e)
			goto fail;
		*calc_checksum = exfat_direntry_checksum(e, *calc_checksum,
							 false);

		if (iloc)
			iloc->disk_offs[nr + 2] = exfat_dctx_dpos(dctx);
		if (name_length < 15)
			len = name_length;

		memcpy(ptr, e->name_frag, len * sizeof (__le16));
		name_length -= len;
		ptr += len;
		nr++;
	}
	return 0;

fail:
	return error;
}

/*
 * walk the directory and invoke filldir on all found entries.
 */
static int __exfat_iterate(struct exfat_dir_ctx *dctx, struct file *file,
			   struct dir_context *ctx)
{
	int error;
	char *name = __getname();
	__le16 *utf16name = __getname();

	if (!name)
		return -ENOMEM;
	if (!utf16name) {
		__putname(name);
		return -ENOMEM;
	}

	for (;;) {
		struct exfat_filedir_entry *efd;
		struct exfat_stream_extension_entry *esx;
		int dtype = DT_REG;
		int name_length;
		bool end;
		u16 calc_checksum;
		u16 expect_checksum;

		/*
		 * get the next filedir entry, we are allowed to skip
		 * entries for that.
		 */
		error = -EIO;
		efd = __exfat_dentry_next(dctx, E_EXFAT_FILEDIR, 0xff,
					  true, &end);
		if (!efd) {
			if (end)
				break;
			else
				goto fail;
		}
		expect_checksum = __le16_to_cpu(efd->set_checksum);
		calc_checksum = exfat_direntry_checksum(efd, 0, true);

		if (__le16_to_cpu(efd->attributes & E_EXFAT_ATTR_DIRECTORY))
			dtype = DT_DIR;

		/*
		 * get immediate stream extension entry.
		 */
		esx = __exfat_dentry_next(dctx, E_EXFAT_STREAM_EXT, 0xff, false,
					  NULL);
		if (!esx)
			goto fail;
		calc_checksum = exfat_direntry_checksum(esx, calc_checksum,
							false);

		/*
		 * get immediate name.
		 */
		error = __exfat_get_name(dctx, esx->name_length, utf16name,
					 &calc_checksum, NULL);
		if (error) {
			exfat_msg(dctx->sb, KERN_INFO, "__exfat_get_name "
				  "has failed with %i", error);
			goto fail;
		}

		if (calc_checksum != expect_checksum) {
			exfat_msg(dctx->sb, KERN_INFO, "checksum: "
				  "calculated %04x, expect %04x",
				  calc_checksum, expect_checksum);
			error = -EIO;
			goto fail;
		}

		/*
		 * convert utf16 to utf8 for kernel filldir callback.
		 */
		name_length = utf16s_to_utf8s(utf16name, esx->name_length,
						   UTF16_LITTLE_ENDIAN,
						   name, NAME_MAX + 2);
		if (name_length < 0) {
			error = name_length;
			goto fail;
		}
		if (name_length > 255) {
			error = -ENAMETOOLONG;
			goto fail;
		}

		/*
		 * tell the kernel we have an entry by calling
		 * dir_emit
		 */
		if (dir_emit(ctx, name, name_length, 1, dtype))
			ctx->pos = 2 + exfat_dctx_fpos(dctx);
		else
			goto fail;
	}
	__putname(name);
	__putname(utf16name);
	ctx->pos = file_inode(file)->i_size + 2;
	return 0;
fail:
	__putname(name);
	__putname(utf16name);
	return error;
}

/*
 * readdir callback for VFS. fill "." and "..", then invoke
 * __exfat_iterate.
 */
int exfat_iterate(struct file *file, struct dir_context *ctx)
{
	struct exfat_dir_ctx dctx;
	int error;
	struct inode *inode = file_inode(file);

	switch (ctx->pos) {
	case 0:
		return dir_emit_dots(file, ctx);
	default:
		if (ctx->pos >= inode->i_size + 2)
			return 0;
		error = exfat_init_dir_ctx(inode, &dctx, ctx->pos - 2);
		if (error)
			return error;
		exfat_lock_super(inode->i_sb);
		error = __exfat_iterate(&dctx, file, ctx);
		exfat_unlock_super(inode->i_sb);
		exfat_cleanup_dir_ctx(&dctx);
		return error;
	}
}
