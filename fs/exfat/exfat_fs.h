/*
 * exfat_fs.h for exfat
 * Created by <nschichan@freebox.fr> on Mon Jul 29 15:06:38 2013
 */

#ifndef __EXFAT_FS_H
# define __EXFAT_FS_H

/*
 * exfat on disk structures and constants
 */

#include <linux/types.h>

struct exfat_vbr {
	u8	jump[3];
	u8	fsname[8];
	u8	reserved1[53];

	__le64	partition_offset;
	__le64	volume_length;

	__le32	fat_offset;
	__le32	fat_length;

	__le32	cluster_heap_offset;
	__le32	cluster_count;
	__le32	cluster_root_dir;

	__le32	serial_number;

	__le16	fs_rev;
	__le16	volume_flags;

	u8	bytes_per_sector;
	u8	sectors_per_cluster;

	u8	fat_num;
	u8	drive_select;
	u8	heap_use_percent;

	u8	reserved2[7];
	u8	boot_code[390];

	u8	boot_sig[2];
};

enum {
	EXFAT_CLUSTER_FIRSTVALID	= 0x00000002,
	EXFAT_CLUSTER_LASTVALID		= 0xfffffff6,
	EXFAT_CLUSTER_BADBLK		= 0xfffffff7,
	EXFAT_CLUSTER_MEDIATYPE		= 0xfffffff8,
	EXFAT_CLUSTER_EOF		= 0xffffffff,
};

enum {
	EXFAT_ACTIVEFAT_MASK = (1 << 0),
	EXFAT_FLAG_DIRTY = (1 << 1),
	EXFAT_FLAG_MEDIA_FAILURE = (1 << 2),
};

static inline int exfat_active_fat(u16 flags)
{
	return flags & EXFAT_ACTIVEFAT_MASK;
}

#define EXFAT_CHECKSUM_SECTORS	11

enum {
	EXFAT_I_ALLOC_POSSIBLE = (1 << 0),
	EXFAT_I_FAT_INVALID = (1 << 1),
};

/*
 * directory cluster content
 */

/*
 * entry types
 */
enum {
	E_EXFAT_EOD		= 0x00,
	E_EXFAT_VOLUME_LABEL	= 0x83,
	E_EXFAT_BITMAP		= 0x81,
	E_EXFAT_UPCASE_TABLE	= 0x82,
	E_EXFAT_GUID		= 0xa0,
	E_EXFAT_PADDING		= 0xa1,
	E_EXFAT_ACL		= 0xe2,
	E_EXFAT_FILEDIR		= 0x85,
	E_EXFAT_STREAM_EXT	= 0xc0,
	E_EXFAT_FILENAME	= 0xc1,
};

/*
 * file attributes in exfat_filedir_entry
 */
enum {
	E_EXFAT_ATTR_RO		= (1 << 0),
	E_EXFAT_ATTR_HIDDEN	= (1 << 1),
	E_EXFAT_ATTR_SYSTEM	= (1 << 2),
	/* bit 3 reserved */
	E_EXFAT_ATTR_DIRECTORY	= (1 << 4),
	E_EXFAT_ATTR_ARCHIVE	= (1 << 5),
	/* bits 6-15 reserved */
};

/* type 0x83 */
struct exfat_volume_label_entry {
	u8 type;
	u8 charcount;
	__u16 label[11];
	u8 reserved1[8];
};

static inline int exfat_bitmap_nr(u8 flags)
{
	return flags & 1;
}

/* type 0x81 */
struct exfat_bitmap_entry {
	u8 type;
	u8 flags;
	u8 reserved1[18];
	__le32 cluster_addr;
	__le64 length;
};

/* type 0x82 */
struct exfat_upcase_entry {
	u8 type;
	u8 reserved1[3];
	__le32 checksum;
	u8 reserved2[12];
	__le32 cluster_addr;
	__le64 length;
};

/* type 0xa0 */
struct exfat_guid_entry {
	u8 type;
	u8 secondary_count;
	__le16 set_checksum;
	__le16 flags;
	u8 guid[16];
	u8 reserved1[10];
};

/* type 0xa1 */
struct exfat_padding_entry {
	u8 type;
	u8 reserved1[31];
};

/* type 0xe2 */
struct exfat_acl_entry {
	u8 type;
	u8 reserved1[31];
};

/* type 0x85 */
struct exfat_filedir_entry {
	u8 type;
	u8 secondary_count;
	__le16 set_checksum;
	__le16 attributes;
	u8 reserved1[2];
	__le32 create;
	__le32 modified;
	__le32 accessed;
	u8 create_10ms;
	u8 modified_10ms;
	s8 create_tz_offset;
	s8 modified_tz_offset;
	s8 accessed_tz_offset;
	u8 reserved2[7];
};

/* 0xc0 */
struct exfat_stream_extension_entry {
	u8 type;
	u8 flags;
	u8 reserved1;
	u8 name_length;
	__le16 name_hash;
	u8 reserved2[2];
	__le64 valid_data_length;
	u8 reserved3[4];
	__le32 first_cluster;
	__le64 data_length;
};

/* 0xc1 */
struct exfat_filename_entry {
	u8 type;
	u8 flags;
	__le16 name_frag[15];
};

#endif /*! __EXFAT_FS_H */
