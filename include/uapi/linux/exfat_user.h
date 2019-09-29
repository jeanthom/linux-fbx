/*
 * exfat_user.h for exfat
 * Created by <nschichan@freebox.fr> on Fri Aug 23 15:31:08 2013
 */

#ifndef __EXFAT_USER_H
# define __EXFAT_USER_H

struct exfat_fragment {
	uint32_t	fcluster_start;
	uint32_t	dcluster_start;
	uint32_t	nr_clusters;
	uint64_t	sector_start;
};

struct exfat_fragment_head {
	uint32_t		fcluster_start;
	uint32_t		nr_fragments;
	uint32_t		sector_size;
	uint32_t		cluster_size;
	struct exfat_fragment	fragments[0];
};

struct exfat_bitmap_data {
	uint32_t		start_cluster;
	uint32_t		nr_clusters;
	uint64_t		sector_start;
	uint64_t		nr_sectors;
};

struct exfat_bitmap_head {
	uint32_t			start_cluster;
	uint32_t			nr_entries;
	struct exfat_bitmap_data	entries[0];
};

struct exfat_dirent_head {
	uint32_t offset;
	uint32_t nr_entries;
	uint8_t entries[0];
};

#define EXFAT_IOCGETFRAGMENTS	_IOR('X', 0x01, struct exfat_fragment_head)
#define EXFAT_IOCGETBITMAP	_IOR('X', 0x02, struct exfat_bitmap_head)
#define EXFAT_IOCGETDIRENTS	_IOR('X', 0x03, struct exfat_dirent_head)

#endif /* !__EXFAT_USER_H */
