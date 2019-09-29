/*
 * exfat.h for exfat
 * Created by <nschichan@freebox.fr> on Tue Jul 23 12:37:12 2013
 */

#ifndef __EXFAT_H
# define __EXFAT_H

#define EXFAT_HASH_BITS	(8)
#define EXFAT_HASH_SIZE	(1 << EXFAT_HASH_BITS)

/*
 * special inode number for root directory.
 */
#define EXFAT_ROOT_INO	1

enum {
	EXFAT_ERROR_ACTION_CONTINUE,
	EXFAT_ERROR_ACTION_REMOUNT_RO,
	EXFAT_ERROR_ACTION_PANIC,
};

struct exfat_sb_options {
	kuid_t	uid;
	kgid_t	gid;
	mode_t	dmask;
	mode_t	fmask;
	int	time_offset;
	int	time_offset_set;
	int	error_action;
};

struct exfat_sb_info {
	struct exfat_sb_options options;

	struct buffer_head *sb_bh;
	struct exfat_vbr *vbr;
	bool dirty;

	u32 sectorsize; /* in bytes*/
	u32 clustersize; /* in bytes */
	u32 sectors_per_cluster;
	int sectorbits;
	int clusterbits;
	u32 sectormask;
	u32 clustermask;

	u32 fat_offset;
	u32 fat_length;

	u32 root_dir_cluster;
	u32 cluster_heap_offset;
	u32 cluster_count;

	__le16	*upcase_table;
	u32	upcase_len;

	/*
	 * bitmap fields
	 */
	struct mutex		bitmap_mutex;
	u32			bitmap_length;
	sector_t		first_bitmap_sector;
	sector_t		last_bitmap_sector;
	sector_t		cur_bitmap_sector;
	u32			cur_bitmap_cluster;
	struct buffer_head	*cur_bitmap_bh;
	u32			free_clusters;
	u32			prev_free_cluster;

	/*
	 * inode hash fields
	 */
	spinlock_t		inode_hash_lock;
	struct hlist_head	inode_hash[EXFAT_HASH_SIZE];

	struct mutex		sb_mutex;
};

struct exfat_cache_entry {
	struct list_head list;
	u32 file_cluster;
	u32 disk_cluster;
	u32 nr_contig;
};

struct exfat_cache {
	struct mutex		mutex;
	struct list_head	entries;
	u32			nr_entries;
};

struct exfat_iloc {
	u8 nr_secondary;
	u32 file_off;
	u64 disk_offs[19];
};

struct exfat_inode_info {
	u8			flags;
	u16			attributes;
	u32			first_cluster;
	u32			allocated_clusters;
	loff_t			mmu_private;
	struct exfat_iloc	iloc;
	struct hlist_node	hash_list;

	struct exfat_cache	exfat_cache;
	struct inode		vfs_inode;
};

static inline struct exfat_sb_info *EXFAT_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct exfat_inode_info *EXFAT_I(struct inode *inode)
{
	return container_of(inode, struct exfat_inode_info, vfs_inode);
}

loff_t exfat_dir_links(struct inode *inode);

int exfat_write_fat_contiguous(struct inode *inode, u32 first_cluster,
			       u32 nr_clusters);
int exfat_write_fat(struct inode *inode, u32 prev_cluster, u32 *clusters,
		    u32 nr_clusters);

__printf(3, 4) void exfat_msg(struct super_block *sb, const char *level,
			      const char *fmt, ...);
__printf(2, 3) void exfat_fs_error(struct super_block *sb,
				   const char *fmt, ...);
int exfat_get_fat_cluster(struct inode *inode, u32 fcluster, u32 *dcluster);
int __exfat_get_fat_cluster(struct inode *inode, u32 fcluster, u32 *dcluster,
			    bool eof_is_fatal);

void exfat_inode_cache_init(struct inode *inode);
void exfat_inode_cache_drop(struct inode *inode);

int exfat_init_fat(struct super_block *sb);

int exfat_init_bitmap(struct inode *root);
void exfat_exit_bitmap(struct super_block *sb);
int exfat_alloc_clusters(struct inode *inode, u32 hint_cluster,
			 u32 *cluster, u32 nr);
int exfat_free_clusters_inode(struct inode *inode, u32 start);


/*
 * read only bitmap accessors: used by EXFAT_IOCGETBITMAP ioctl.
 */
struct exfat_bitmap_ctx {
	struct super_block *sb;
	struct buffer_head *bh;
	sector_t cur_sector;
};

int exfat_init_bitmap_context(struct super_block *sb,
			      struct exfat_bitmap_ctx *ctx, u32 cluster);
void exfat_exit_bitmap_context(struct exfat_bitmap_ctx *ctx);
int exfat_test_bitmap(struct exfat_bitmap_ctx *ctx, uint32_t start_cluster,
		      uint32_t *first_in_use, uint32_t *nr_in_use);


/*
 * return the physical sector address for a given cluster.
 */
static inline sector_t exfat_cluster_sector(struct exfat_sb_info *sbi,
					    u32 cluster)
{
	return (sector_t)sbi->cluster_heap_offset + (cluster - 2) *
		(sector_t)sbi->sectors_per_cluster;
}

/*
 * in dir.c
 */
struct exfat_dir_ctx {
	struct super_block	*sb;
	struct inode		*inode;
	struct buffer_head	*bh;

	off_t			off; /* from beginning of directory */
	sector_t		sector;
	bool empty;
};

int exfat_init_dir_ctx(struct inode *inode, struct exfat_dir_ctx *ctx,
		       off_t off);
void exfat_cleanup_dir_ctx(struct exfat_dir_ctx *dctx);
int exfat_get_cluster_hint(struct inode *inode, u32 *out_hint);
int exfat_dentry_next(void *, struct exfat_dir_ctx *, int, bool);
void *__exfat_dentry_next(struct exfat_dir_ctx *dctx, int type, int mask,
			  bool can_skip, bool *end);
u16 exfat_direntry_checksum(void *data, u16 checksum, bool first);
u32 exfat_dctx_fpos(struct exfat_dir_ctx *dctx);
u64 exfat_dctx_dpos(struct exfat_dir_ctx *dctx);
int __exfat_get_name(struct exfat_dir_ctx *dctx, u32 name_length, __le16 *name,
		     u16 *calc_checksum, struct exfat_iloc *iloc);

/*
 * in namei.c
 */

/*
 * hold a pointer to an exfat dir entry, with the corresponding bh.
 */
struct dir_entry_buffer {
	struct buffer_head *bh;
	u32 off; /* in bytes, inside the buffer_head b_data array */
	void *start;
};

int exfat_get_dir_entry_buffers(struct inode *dir, struct exfat_iloc *iloc,
				struct dir_entry_buffer *entries,
				size_t nr_entries);
u16 exfat_dir_entries_checksum(struct dir_entry_buffer *entries, u32 nr);
void exfat_dirty_dir_entries(struct dir_entry_buffer *entries,
			     size_t nr_entries, bool sync);
void exfat_write_time(struct exfat_sb_info *sbi, struct timespec *ts,
		      __le32 *datetime, u8 *time_cs, u8 *tz_offset);

/*
 * in inode.c
 */

int exfat_init_inodes(void);
void exfat_exit_inodes(void);

struct inode *exfat_iget(struct super_block *sb, loff_t disk_pos);
void exfat_insert_inode_hash(struct inode *inode);
void exfat_remove_inode_hash(struct inode *inode);
int __exfat_write_inode(struct inode *inode, bool sync);

/*
 * in upcase.c
 */
int exfat_upcase_init(struct inode *root);
static inline __le16 exfat_upcase_convert(struct super_block *sb, __le16 _c)
{
	u16 c = __le16_to_cpu(_c);

	if (c >= EXFAT_SB(sb)->upcase_len)
		return _c;
	return EXFAT_SB(sb)->upcase_table[c];
}

/*
 * superblock operations
 */
struct inode *exfat_alloc_inode(struct super_block *sb);
void exfat_destroy_inode(struct inode *_inode);
int exfat_drop_inode(struct inode *inode);
void exfat_evict_inode(struct inode *inode);

/*
 * file operations
 */
int exfat_iterate(struct file *f, struct dir_context *ctx);
long exfat_ioctl(struct file *, unsigned int, unsigned long);
int exfat_truncate_blocks(struct inode *inode, loff_t newsize);

/*
 * inode operations
 */
struct dentry *exfat_inode_lookup(struct inode *, struct dentry *,
				  unsigned int);
int exfat_inode_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool excl);
int exfat_inode_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);

mode_t exfat_make_mode(struct exfat_sb_info *sbi, mode_t mode, u16 attrs);

int exfat_write_inode(struct inode *inode, struct writeback_control *wbc);

int exfat_inode_unlink(struct inode *inode, struct dentry *dentry);

int exfat_inode_rmdir(struct inode *inode, struct dentry *dentry);

int exfat_getattr(struct vfsmount *, struct dentry *, struct kstat *);
int exfat_setattr(struct dentry *, struct iattr *);
int exfat_rename(struct inode *, struct dentry *,
		 struct inode *, struct dentry *);

/*
 * address space operations
 */
int exfat_readpage(struct file *file, struct page *page);
int exfat_readpages(struct file *file, struct address_space *mapping,
		    struct list_head *pages, unsigned nr_pages);
int exfat_write_begin(struct file *file, struct address_space *mapping,
		      loff_t pos, unsigned len, unsigned flags,
		      struct page **pagep, void **fsdata);
int exfat_write_end(struct file *file, struct address_space *mapping,
		    loff_t pos, unsigned len, unsigned copied,
		    struct page *page, void *fsdata);
int exfat_writepage(struct page *page, struct writeback_control *wbc);
int exfat_writepages(struct address_space *, struct writeback_control *);


extern const struct inode_operations exfat_dir_inode_operations;
extern const struct inode_operations exfat_file_inode_operations;
extern const struct file_operations exfat_dir_operations;
extern const struct file_operations exfat_file_operations;
extern const struct address_space_operations exfat_address_space_operations;

/*
 * time functions
 */
void exfat_time_2unix(struct timespec *ts, u32 datetime, u8 time_cs,
		      s8 tz_offset);
void exfat_time_2exfat(struct exfat_sb_info *sbi, struct timespec *ts,
		       u32 *datetime, u8 *time_cs, s8 *tz_offset);

static inline void exfat_lock_super(struct super_block *sb)
{
	mutex_lock(&EXFAT_SB(sb)->sb_mutex);
}

static inline void exfat_unlock_super(struct super_block *sb)
{
	mutex_unlock(&EXFAT_SB(sb)->sb_mutex);
}

#endif /*! __EXFAT_H */
