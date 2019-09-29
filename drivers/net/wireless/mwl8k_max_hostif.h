#ifndef HOSTIF_H_
#define HOSTIF_H_

#ifndef __KERNEL__
/*
 * map to linux kernel types for easier sharing
 */
#include "linux_types.h"
#endif

/*
 * device => host IRQ
 */
#define IRQ_D2H_VUART	(1 << 0)

/*
 * IX ZONE
 */
#define IXZONE_MAGIC	0x2cc4189b

enum {
	IXZONE_VUART,
};

struct ixzone_root {
	__le32		magic;
	__le32		node_count;
};

struct ixzone_node {
	__le32		type;

	/* relative to root */
	__le32		off;
};

struct vuart_ixzone {
	__le32		buf_offset;
	__le32		buf_size;

	/*
	 * 'fw' & 'host' are relative (0 < val < buf_size)
	 *
	 * firmware increments 'fw', host increments 'host',
	 *
	 * (fw == host) => buffer is empty
	 * (fw == host - 1) => buffer is full
	 */
	__le32		fw;
	__le32		host;
};

#endif /* ! HOSTIF_H_ */
