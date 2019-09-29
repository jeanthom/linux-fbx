/*
 * namei.c for exfat
 * Created by <nschichan@freebox.fr> on Tue Aug 20 12:00:27 2013
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/nls.h>

#include "exfat.h"
#include "exfat_fs.h"

static u16 exfat_filename_hash_cont(struct super_block *sb,
				    const __le16 *name, u16 hash, size_t len);


void exfat_write_time(struct exfat_sb_info *sbi, struct timespec *ts,
		      __le32 *datetime, u8 *time_cs, u8 *tz_offset)
{
	u32 cpu_datetime;

	exfat_time_2exfat(sbi, ts, &cpu_datetime, time_cs, tz_offset);
	*datetime = __cpu_to_le32(cpu_datetime);
}

static void exfat_read_time(struct timespec *ts, __le32 datetime, u8 time_cs,
			    u8 tz_offset)
{
	u32 cpu_datetime = __le32_to_cpu(datetime);
	exfat_time_2unix(ts, cpu_datetime, time_cs, tz_offset);
}

static int exfat_zero_cluster(struct super_block *sb, u32 cluster, bool sync)
{
	sector_t start = exfat_cluster_sector(EXFAT_SB(sb), cluster);
	sector_t end = start + EXFAT_SB(sb)->sectors_per_cluster;
	sector_t sect;

	for (sect = start; sect < end; ++sect) {
		struct buffer_head *bh = sb_bread(sb, sect);
		if (!bh) {
			exfat_msg(sb, KERN_WARNING,
				  "unable to read sector %llu for zeroing.",
				  sect);
			return -EIO;
		}
		memset(bh->b_data, 0, bh->b_size);
		mark_buffer_dirty(bh);
		if (sync)
			sync_dirty_buffer(bh);
		brelse(bh);
	}
	return 0;
}

/*
 * use per superblock fmask or dmaks, depending on provided entry
 * attribute to restrict the provided mode even more.
 */
mode_t exfat_make_mode(struct exfat_sb_info *sbi, mode_t mode, u16 attrs)
{
	if (attrs & E_EXFAT_ATTR_DIRECTORY)
		mode = (mode & ~sbi->options.dmask) | S_IFDIR;
	else
		mode = (mode & ~sbi->options.fmask) | S_IFREG;
	if (attrs & E_EXFAT_ATTR_RO)
		mode &= ~S_IWUGO;
	return mode;
}

/*
 * populate inode fields.
 */
static struct inode *exfat_populate_inode(struct super_block *sb,
			  const struct exfat_filedir_entry *efd,
			  const struct exfat_stream_extension_entry *esx,
			  const struct exfat_iloc *iloc)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct inode *inode;

	inode = exfat_iget(sb, iloc->disk_offs[0]);
	if (inode)
		return inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = iunique(sb, EXFAT_ROOT_INO);
	EXFAT_I(inode)->first_cluster = __le32_to_cpu(esx->first_cluster);
	EXFAT_I(inode)->flags = esx->flags;
	EXFAT_I(inode)->iloc = *iloc;
	EXFAT_I(inode)->attributes = __le16_to_cpu(efd->attributes);

	inode->i_size = __le64_to_cpu(esx->data_length);
	EXFAT_I(inode)->allocated_clusters = inode->i_size >> sbi->clusterbits;
	if (inode->i_size & sbi->clustermask)
		EXFAT_I(inode)->allocated_clusters++;
	inode->i_blocks = EXFAT_I(inode)->allocated_clusters <<
		(sbi->clusterbits - 9);
	EXFAT_I(inode)->mmu_private = inode->i_size;

	inode->i_uid = sbi->options.uid;
	inode->i_gid = sbi->options.gid;
	inode->i_mode = exfat_make_mode(sbi, S_IRWXUGO,
					EXFAT_I(inode)->attributes);

	if (EXFAT_I(inode)->attributes & E_EXFAT_ATTR_DIRECTORY) {
		loff_t nlinks = exfat_dir_links(inode);
		if (nlinks < 0)
			goto iput;
		set_nlink(inode, nlinks + 2);
	} else
		set_nlink(inode, 1);

	if (esx->data_length != esx->valid_data_length)
		exfat_msg(sb, KERN_WARNING, "data length (%llu) != valid data "
			  "length (%llu)", __le64_to_cpu(esx->data_length),
			  __le64_to_cpu(esx->valid_data_length));

	if (S_ISDIR(inode->i_mode)) {
		inode->i_fop = &exfat_dir_operations;
		inode->i_op = &exfat_dir_inode_operations;
	} else {
		/* until we support write */
		inode->i_fop = &exfat_file_operations;
		inode->i_op = &exfat_file_inode_operations;
		inode->i_data.a_ops = &exfat_address_space_operations;
	}


	exfat_read_time(&inode->i_ctime, efd->create, efd->create_10ms,
			efd->create_tz_offset);
	exfat_read_time(&inode->i_mtime, efd->modified, efd->modified_10ms,
			efd->modified_tz_offset);
	exfat_read_time(&inode->i_atime, efd->accessed, 0,
			efd->accessed_tz_offset);

	exfat_insert_inode_hash(inode);
	insert_inode_hash(inode);
	return inode;
iput:
	iput(inode);
	return NULL;
}

/*
 * lookup an inode.
 */
struct dentry *exfat_inode_lookup(struct inode *parent, struct dentry *dentry,
				  unsigned int flags)
{
	struct super_block *sb = dentry->d_sb;
	struct exfat_dir_ctx dctx;
	int error;
	struct exfat_filedir_entry efd;
	struct exfat_stream_extension_entry esx;
	__le16 *name = __getname();
	__le16 *utf16_name = __getname();
	unsigned int utf16_name_length;
	__le16 name_hash;

	exfat_lock_super(parent->i_sb);

	if (!name || !utf16_name) {
		error = -ENOMEM;
		goto putnames;
	}

	utf16_name_length = utf8s_to_utf16s(dentry->d_name.name,
					    dentry->d_name.len,
					    UTF16_LITTLE_ENDIAN,
					    utf16_name, 255 + 2);
	if (utf16_name_length > 255) {
		error = -ENAMETOOLONG;
		goto putnames;
	}

	/*
	 * get the name hash of the wanted inode early so that we can
	 * skip entries with only an efd and an esx entry.
	 */
	name_hash = __cpu_to_le16(exfat_filename_hash_cont(sb, utf16_name, 0,
							   utf16_name_length));

	/*
	 * create a dir ctx from the parent so that we can iterate on
	 * it.
	 */
	error = exfat_init_dir_ctx(parent, &dctx, 0);
	if (error)
		goto putnames;

	for (;;) {
		u32 name_length;
		struct inode *inode;
		u16 calc_checksum;
		u16 expect_checksum;
		struct exfat_iloc iloc;

		memset(&iloc, 0, sizeof (iloc));
		/*
		 * get filedir and stream extension entries.
		 */
		error = exfat_dentry_next(&efd, &dctx, E_EXFAT_FILEDIR, true);
		if (error < 0)
			/* end of directory reached, or other error */
			goto cleanup;

		error = -EINVAL;
		if (efd.secondary_count > 18)
			goto cleanup;

		iloc.file_off = exfat_dctx_fpos(&dctx);
		iloc.disk_offs[0] = exfat_dctx_dpos(&dctx);
		iloc.nr_secondary = efd.secondary_count + 1;

		error = exfat_dentry_next(&esx, &dctx, E_EXFAT_STREAM_EXT,
					  false);
		if (error)
			goto cleanup;

		if (esx.name_hash != name_hash)
			/*
			 * stored name hash is not the same as the
			 * wanted hash: no point in processing the
			 * remaining entries for the current efd/esx
			 * any further.
			 */
			continue ;

		/*
		 * now that the hash matches it is ok to update the
		 * checksum for the efd and esx entries.
		 */
		expect_checksum = __le16_to_cpu(efd.set_checksum);
		calc_checksum = exfat_direntry_checksum(&efd, 0, true);

		calc_checksum = exfat_direntry_checksum(&esx,
							calc_checksum, false);
		iloc.disk_offs[1] = exfat_dctx_dpos(&dctx);

		/*
		 * fetch name.
		 */
		name_length = esx.name_length;
		error = __exfat_get_name(&dctx, name_length, name,
					 &calc_checksum, &iloc);
		if (error)
			goto cleanup;

		if (calc_checksum != expect_checksum) {
			exfat_msg(dctx.sb, KERN_INFO, "checksum: "
				  "calculated %04x, expect %04x",
				  calc_checksum, expect_checksum);
			error = -EIO;
			goto cleanup;
		}


		if (utf16_name_length != name_length)
			continue ;

		if (memcmp(utf16_name, name, name_length * sizeof (__le16)))
			continue ;

		inode = exfat_populate_inode(sb, &efd, &esx, &iloc);
		if (inode) {
			d_add(dentry, inode);
			error = 0;
		} else
			error = -EIO;
		goto cleanup;
	}

cleanup:
	exfat_cleanup_dir_ctx(&dctx);
putnames:
	if (name)
		__putname(name);
	if (utf16_name)
		__putname(utf16_name);
	exfat_unlock_super(parent->i_sb);
	if (error && error != -ENOENT)
		return ERR_PTR(error);
	return NULL;
}

/*
 * find nr unused directory entries (type & 0x80 == 0).
 */
static int exfat_find_dir_iloc(struct inode *inode, int nr,
			       struct exfat_iloc *iloc)
{
	struct exfat_dir_ctx dctx;
	bool end = false;
	int error;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	u32 nr_new_clusters, i;
	u32 new_clusters[2];
	u32 hint_cluster;

retry:
	memset(iloc, 0, sizeof (*iloc));
	iloc->nr_secondary = nr;

	error = exfat_init_dir_ctx(inode, &dctx, 0);
	if (error)
		return error;

	while (1) {
		int nr_free;
		void *ent;

		ent = __exfat_dentry_next(&dctx, 0x00, 0x80, true, &end);
		if (end)
			break;
		if (!ent) {
			exfat_cleanup_dir_ctx(&dctx);
			return -EIO;
		}

		nr_free = 1;
		iloc->file_off = exfat_dctx_fpos(&dctx);
		iloc->disk_offs[0] = exfat_dctx_dpos(&dctx);
		while (__exfat_dentry_next(&dctx, 0x00, 0x80, false, &end)
		       != NULL && nr_free < nr) {
			iloc->disk_offs[nr_free] = exfat_dctx_dpos(&dctx);
			++nr_free;
		}
		if (nr_free == nr) {
			/*
			 * we found enough consecutive free entries.
			 */
			exfat_cleanup_dir_ctx(&dctx);
			return 0;
		}

	}

	/*
	 * not enough consecutive free entries found, kick the cluster
	 * allocator and retry.
	 */
	exfat_cleanup_dir_ctx(&dctx);

	/*
	 * with the smallest cluster size, a file can take more than
	 * two clusters. allocate two in that case reardless of what
	 * is needed to make code simplier.
	 */
	switch (sbi->clustersize) {
	case 512:
		nr_new_clusters = 2;
		break;
	default:
		nr_new_clusters = 1;
		break;
	}

	/*
	 * get a hint cluster for the cluster allocator.
	 */
	error = exfat_get_cluster_hint(inode, &hint_cluster);
	if (error)
		return error;

	/*
	 * peform the allocation.
	 */
	error = exfat_alloc_clusters(inode, hint_cluster, new_clusters,
				     nr_new_clusters);
	if (error)
		return error;

	/*
	 * fill new cluster(s) with zero.
	 */
	for (i = 0; i < nr_new_clusters; ++i)
		exfat_zero_cluster(inode->i_sb, new_clusters[i], false);

	/*
	 * update size and mark inode as dirty so that write_inode()
	 * can update it's size, and the other fields updated by
	 * exfat_alloc_clusters.
	 */
	inode->i_size += nr_new_clusters << sbi->clusterbits;
	mark_inode_dirty(inode);

	/*
	 * kick the whole place search again, this time with the newly
	 * allocated clusters.
	 */
	goto retry;
}

/*
 * setup dir_entry_buffers starting at using iloc.
 */
int exfat_get_dir_entry_buffers(struct inode *dir, struct exfat_iloc *iloc,
				struct dir_entry_buffer *entries,
				size_t nr_entries)
{
	size_t i;
	int error;
	struct exfat_sb_info *sbi = EXFAT_SB(dir->i_sb);

	BUG_ON(iloc->nr_secondary != nr_entries);

	memset(entries, 0, sizeof (*entries) * nr_entries);
	for (i = 0; i < nr_entries; ++i) {
		sector_t sector = iloc->disk_offs[i] >> sbi->sectorbits;

		entries[i].off = iloc->disk_offs[i] & sbi->sectormask;
		entries[i].bh = sb_bread(dir->i_sb, sector);
		if (!entries[i].bh) {
			error = -EIO;
			goto fail;
		}
		entries[i].start = entries[i].bh->b_data + entries[i].off;
	}
	return 0;

fail:
	for (i = 0; i < nr_entries; ++i)
		if (entries[i].bh)
			brelse(entries[i].bh);
	return error;
}

static u16 exfat_filename_hash_cont(struct super_block *sb,
				    const __le16 *name, u16 hash, size_t len)
{
	while (len) {
		u16 c = __le16_to_cpu(exfat_upcase_convert(sb, *name));

		hash = ((hash << 15) | (hash >> 1)) + (c & 0xff);
		hash = ((hash << 15) | (hash >> 1)) + (c >> 8);
		--len;
		++name;
	}
	return hash;
}

u16 exfat_dir_entries_checksum(struct dir_entry_buffer *entries, u32 nr)
{
	u32 checksum = 0;

	if (nr) {
		checksum = exfat_direntry_checksum(entries->start,
						   checksum, true);
		--nr;
		++entries;
	}
	while (nr) {
		checksum = exfat_direntry_checksum(entries->start,
						   checksum, false);
		--nr;
		++entries;
	}
	return checksum;
}

/*
 * setup exfat_filedir_entry and exfat_stream_extension_entry for a
 * new entry, with attribute attrs, and named name.
 */
static void exfat_fill_dir_entries(struct super_block *sb,
				  struct dir_entry_buffer *entries,
				  size_t nr_entries, u8 attrs,
				  __le16 *name, int name_length)
{
	struct exfat_filedir_entry *efd;
	struct exfat_stream_extension_entry *esx;
	int i;
	u16 name_hash;
	u16 checksum;
	struct timespec ts = CURRENT_TIME_SEC;

	efd = entries[0].start;
	esx = entries[1].start;

	/*
	 * fill exfat filedir entry
	 */
	memset(efd, 0, sizeof (*efd));
	efd->type = E_EXFAT_FILEDIR;
	efd->secondary_count = nr_entries - 1;
	efd->set_checksum = 0;
	efd->attributes = __cpu_to_le16(attrs);

	/*
	 * update file directory entry times
	 */
	efd = entries[0].start;
	exfat_write_time(EXFAT_SB(sb), &ts, &efd->create, &efd->create_10ms,
			 &efd->create_tz_offset);
	efd->modified = efd->accessed = efd->create;
	efd->modified_10ms = efd->create_10ms;
	efd->accessed_tz_offset = efd->modified_tz_offset =
		efd->create_tz_offset;

	/*
	 * fill exfat stream extension entry
	 */
	memset(esx, 0, sizeof (*esx));
	esx->type = E_EXFAT_STREAM_EXT;
	esx->flags = EXFAT_I_ALLOC_POSSIBLE;
	esx->first_cluster = __cpu_to_le32(0);
	esx->data_length = __cpu_to_le64(0);
	esx->valid_data_length = __cpu_to_le64(0);
	esx->name_length = name_length;

	/*
	 * fill name fragments.
	 */
	name_hash = 0;
	for (i = 0; i < nr_entries - 2; ++i, name_length -= 15) {
		struct exfat_filename_entry *efn = entries[i + 2].start;
		int len = 15;

		if (name_length < 15)
			len = name_length;

		memset(efn, 0, sizeof (*efn));
		efn->type = E_EXFAT_FILENAME;
		memcpy(efn->name_frag, name + i * 15, len * sizeof (__le16));
		name_hash = exfat_filename_hash_cont(sb, efn->name_frag,
						     name_hash, len);
	}
	esx->name_hash = __cpu_to_le16(name_hash);

	checksum = exfat_dir_entries_checksum(entries, nr_entries);
	efd->set_checksum = __cpu_to_le16(checksum);
}

/*
 * mark all buffer heads in the entries array as dirty. optionally
 * sync them if required.
 */
void exfat_dirty_dir_entries(struct dir_entry_buffer *entries,
			     size_t nr_entries, bool sync)
{
	size_t i;

	for (i = 0; i < nr_entries; ++i) {
		mark_buffer_dirty(entries[i].bh);
		if (sync)
			sync_dirty_buffer(entries[i].bh);
		brelse(entries[i].bh);
	}
}

/*
 * cleanup all buffer heads in entries.
 */
static void exfat_cleanup_dir_entries(struct dir_entry_buffer *entries,
				     size_t nr_entries)
{
	size_t i;

	for (i = 0; i < nr_entries; ++i)
		brelse(entries[i].bh);
}

/*
 * create an inode
 */
static int __exfat_inode_create(struct inode *dir, struct dentry *dentry,
				umode_t mode, bool is_dir)
{
	int nr_entries;
	struct dir_entry_buffer entries[19];
	struct inode *new;
	struct exfat_iloc iloc;
	int error;
	u8 attr = 0;
	__le16 *utf16_name;
	int utf16_name_length;

	if (is_dir)
		attr |= E_EXFAT_ATTR_DIRECTORY;

	exfat_lock_super(dir->i_sb);

	utf16_name = __getname();
	if (!utf16_name) {
		error = -ENOMEM;
		goto unlock_super;
	}

	utf16_name_length = utf8s_to_utf16s(dentry->d_name.name,
					    dentry->d_name.len,
					    UTF16_LITTLE_ENDIAN, utf16_name,
					    255 + 2);
	if (utf16_name_length < 0) {
		error = utf16_name_length;
		goto putname;
	}
	if (utf16_name_length > 255) {
		error = -ENAMETOOLONG;
		goto putname;
	}


	nr_entries = 2 + DIV_ROUND_UP(utf16_name_length, 15);
	if (nr_entries > 19) {
		error = -ENAMETOOLONG;
		goto putname;
	}

	error = exfat_find_dir_iloc(dir, nr_entries, &iloc);
	if (error < 0)
		goto putname;

	error = exfat_get_dir_entry_buffers(dir, &iloc, entries, nr_entries);
	if (error)
		goto putname;
	exfat_fill_dir_entries(dir->i_sb, entries, nr_entries, attr,
				       utf16_name, utf16_name_length);

	/*
	 * create an inode with it.
	 */
	error = -ENOMEM;
	new = exfat_populate_inode(dir->i_sb, entries[0].start,
				   entries[1].start, &iloc);
	if (!new)
		goto cleanup;
	inc_nlink(dir);
	d_instantiate(dentry, new);

	/*
	 * update directory atime / ctime.
	 */
	dir->i_atime = dir->i_mtime = CURRENT_TIME_SEC;
	if (IS_DIRSYNC(dir))
		__exfat_write_inode(dir, true);
	else
		mark_inode_dirty(dir);

	/*
	 * write to disk
	 */
	exfat_dirty_dir_entries(entries, nr_entries, false);
	__putname(utf16_name);
	exfat_unlock_super(dir->i_sb);
	return 0;

cleanup:
	exfat_cleanup_dir_entries(entries, nr_entries);
putname:
	__putname(utf16_name);
unlock_super:
	exfat_unlock_super(dir->i_sb);
	return error;
}

int exfat_inode_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool excl)
{
	return __exfat_inode_create(dir, dentry, mode, false);
}

int exfat_inode_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return __exfat_inode_create(dir, dentry, mode, true);
}

/*
 * inode unlink: find all direntry buffers and clear seventh bit of
 * the entry type to mark the as unused.
 */
static int __exfat_inode_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct exfat_inode_info *info = EXFAT_I(inode);
	struct dir_entry_buffer entries[info->iloc.nr_secondary];
	int error;
	u32 i;

	error = exfat_get_dir_entry_buffers(inode, &info->iloc,
					    entries, info->iloc.nr_secondary);
	if (error)
		return error;

	for (i = 0; i < info->iloc.nr_secondary; ++i) {
		u8 *type = entries[i].start;

		*type &= 0x7f;
	}

	drop_nlink(dir);
	clear_nlink(inode);
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;

	/*
	 * update atime & mtime for parent directory.
	 */
	dir->i_mtime = dir->i_atime = CURRENT_TIME_SEC;
	if (IS_DIRSYNC(dir))
		__exfat_write_inode(dir, true);
	else
		mark_inode_dirty(dir);

	exfat_dirty_dir_entries(entries, info->iloc.nr_secondary, false);
	exfat_remove_inode_hash(inode);
	return 0;
}

int exfat_inode_unlink(struct inode *dir, struct dentry *dentry)
{
	int ret;

	exfat_lock_super(dir->i_sb);
	ret = __exfat_inode_unlink(dir, dentry);
	exfat_unlock_super(dir->i_sb);
	return ret;
}

/*
 * inode rmdir: check that links is not greater than 2 (meaning that
 * the directory is empty) and invoke unlink.
 */
static int __exfat_inode_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	if (inode->i_nlink > 2)
		return -ENOTEMPTY;

	return __exfat_inode_unlink(dir, dentry);
}

int exfat_inode_rmdir(struct inode *dir, struct dentry *dentry)
{
	int ret;

	exfat_lock_super(dir->i_sb);
	ret = __exfat_inode_rmdir(dir, dentry);
	exfat_unlock_super(dir->i_sb);
	return ret;
}

int exfat_rename(struct inode *old_dir, struct dentry *old_dentry,
		 struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	int new_nr_entries;
	int error = 0;
	struct exfat_iloc new_iloc;
	struct exfat_inode_info *old_info = EXFAT_I(old_inode);
	struct dir_entry_buffer old_buffers[old_info->iloc.nr_secondary];
	struct dir_entry_buffer new_buffers[19];
	struct exfat_filedir_entry *efd;
	struct exfat_stream_extension_entry *esx;
	int name_length;
	__le16 *name;
	u16 name_hash;
	int i;

	exfat_lock_super(new_dir->i_sb);

	/*
	 * convert new name to utf16
	 */
	name = __getname();
	if (!name) {
		error = -ENOMEM;
		goto unlock_super;
	}
	name_length = utf8s_to_utf16s(new_dentry->d_name.name,
				      new_dentry->d_name.len,
				      UTF16_LITTLE_ENDIAN, name, 255 + 2);

	if (name_length > 255) {
		error = -ENAMETOOLONG;
		goto err_putname;
	}
	if (name_length < 0) {
		error = name_length;
		goto err_putname;
	}

	new_nr_entries = 2 + DIV_ROUND_UP(name_length, 15);

	/*
	 * find space for new entry
	 */
	error = exfat_find_dir_iloc(new_dir, new_nr_entries, &new_iloc);
	if (error < 0)
		goto err_putname;

	/*
	 * get buffers for old and new entries.
	 */
	error = exfat_get_dir_entry_buffers(old_dir, &old_info->iloc,
				    old_buffers, old_info->iloc.nr_secondary);
	if (error < 0)
		goto err_putname;

	error = exfat_get_dir_entry_buffers(new_dir, &new_iloc, new_buffers,
					    new_nr_entries);
	if (error < 0)
		goto err_cleanup_old_buffers;


	/*
	 * remove new inode, if it exists.
	 */
	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode))
			error = __exfat_inode_rmdir(new_dir, new_dentry);
		else
			error = __exfat_inode_unlink(new_dir, new_dentry);
		if (error < 0)
			goto err_cleanup_new_buffers;
	}

	/*
	 * move old esd to new esd (and ditto for esx).
	 */
	efd = new_buffers[0].start;
	esx = new_buffers[1].start;
	memcpy(efd, old_buffers[0].start, sizeof (*efd));
	memcpy(esx, old_buffers[1].start, sizeof (*esx));

	efd->secondary_count = new_nr_entries - 1;

	/*
	 * patch new name after that.
	 */
	esx->name_length = __cpu_to_le16(name_length);

	/*
	 * fill name fragments.
	 */
	name_hash = 0;
	for (i = 0; i < new_nr_entries - 2; ++i, name_length -= 15) {
		struct exfat_filename_entry *efn = new_buffers[i + 2].start;
		int len = 15;

		if (name_length < 15)
			len = name_length;

		memset(efn, 0, sizeof (*efn));
		efn->type = E_EXFAT_FILENAME;
		memcpy(efn->name_frag, name + i * 15, len * sizeof (__le16));
		name_hash = exfat_filename_hash_cont(new_dir->i_sb,
						     efn->name_frag,
						     name_hash, len);
	}
	__putname(name);
	esx->name_hash = __cpu_to_le16(name_hash);
	efd->set_checksum = exfat_dir_entries_checksum(new_buffers,
						       new_nr_entries);
	efd->set_checksum = __cpu_to_le16(efd->set_checksum);

	/*
	 * mark old buffer entries as unused.
	 */
	for (i = 0; i < old_info->iloc.nr_secondary; ++i)
		*((u8*)old_buffers[i].start) &= 0x7f;

	/*
	 * dirty old & new entries buffers.
	 */
	exfat_dirty_dir_entries(new_buffers, new_nr_entries, false);
	exfat_dirty_dir_entries(old_buffers, old_info->iloc.nr_secondary,
				false);

	/*
	 * update links if new_dir and old_dir are differents.
	 */
	if (new_dir != old_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	/*
	 * make old inode use the new iloc, and update sb inode hash.
	 */
	exfat_remove_inode_hash(old_inode);
	old_info->iloc = new_iloc;
	exfat_insert_inode_hash(old_inode);

	/*
	 * update new dir & old dir mtime/atime
	 */
	if (new_dir == old_dir) {
		new_dir->i_mtime = new_dir->i_atime = CURRENT_TIME_SEC;
		if (IS_DIRSYNC(new_dir))
			__exfat_write_inode(new_dir, true);
		else
			mark_inode_dirty(new_dir);
	} else {
		new_dir->i_mtime = new_dir->i_atime =
			old_dir->i_mtime = old_dir->i_atime = CURRENT_TIME_SEC;
		if (IS_DIRSYNC(new_dir)) {
			__exfat_write_inode(new_dir, true);
			__exfat_write_inode(old_dir, true);
		} else {
			mark_inode_dirty(new_dir);
			mark_inode_dirty(old_dir);
		}
	}

	exfat_unlock_super(new_dir->i_sb);
	return 0;

err_cleanup_new_buffers:
	exfat_cleanup_dir_entries(new_buffers, new_nr_entries);
err_cleanup_old_buffers:
	exfat_cleanup_dir_entries(old_buffers, old_info->iloc.nr_secondary);
err_putname:
	__putname(name);
unlock_super:
	exfat_unlock_super(new_dir->i_sb);
	return error;
}
