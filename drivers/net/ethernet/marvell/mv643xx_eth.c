/*
 * Driver for Marvell Discovery (MV643XX) and Marvell Orion ethernet ports
 * Copyright (C) 2002 Matthew Dharm <mdharm@momenco.com>
 *
 * Based on the 64360 driver from:
 * Copyright (C) 2002 Rabeeh Khoury <rabeeh@galileo.co.il>
 *		      Rabeeh Khoury <rabeeh@marvell.com>
 *
 * Copyright (C) 2003 PMC-Sierra, Inc.,
 *	written by Manish Lachwani
 *
 * Copyright (C) 2003 Ralf Baechle <ralf@linux-mips.org>
 *
 * Copyright (C) 2004-2006 MontaVista Software, Inc.
 *			   Dale Farnsworth <dale@farnsworth.org>
 *
 * Copyright (C) 2004 Steven J. Hill <sjhill1@rockwellcollins.com>
 *				     <sjhill@realitydiluted.com>
 *
 * Copyright (C) 2007-2008 Marvell Semiconductor
 *			   Lennert Buytenhek <buytenh@marvell.com>
 *
 * Copyright (C) 2013 Michael Stapelberg <michael@stapelberg.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <net/tso.h>
#include <net/arp.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/phy.h>
#include <linux/mv643xx_eth.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/if_vlan.h>
#include <linux/sort.h>
#include <linux/fbxbridge.h>
#include "../../../net/bridge/br_private.h"

#ifdef CONFIG_MV643XX_ETH_FBX_FF
#include <net/ip_ffn.h>
#include <net/ip_tunnels.h>
#include <net/ip6_ffn.h>
#include <net/ip6_route.h>
#include <net/ip6_tunnel.h>
#endif

static char mv643xx_eth_driver_name[] = "mv643xx_eth";
static char mv643xx_eth_driver_version[] = "1.4";


/*
 * Registers shared between all ports.
 */
#define PHY_ADDR			0x0000
#define WINDOW_BASE(w)			(0x0200 + ((w) << 3))
#define WINDOW_SIZE(w)			(0x0204 + ((w) << 3))
#define WINDOW_REMAP_HIGH(w)		(0x0280 + ((w) << 2))
#define WINDOW_BAR_ENABLE		0x0290
#define WINDOW_PROTECT(w)		(0x0294 + ((w) << 4))

/*
 * Main per-port registers.  These live at offset 0x0400 for
 * port #0, 0x0800 for port #1, and 0x0c00 for port #2.
 */
#define PORT_CONFIG			0x0000
#define  UNICAST_PROMISCUOUS_MODE	0x00000001
#define PORT_CONFIG_EXT			0x0004
#define MAC_ADDR_LOW			0x0014
#define MAC_ADDR_HIGH			0x0018
#define SDMA_CONFIG			0x001c
#define  TX_BURST_SIZE_16_64BIT		0x01000000
#define  TX_BURST_SIZE_4_64BIT		0x00800000
#define  BLM_TX_NO_SWAP			0x00000020
#define  BLM_RX_NO_SWAP			0x00000010
#define  RX_BURST_SIZE_16_64BIT		0x00000008
#define  RX_BURST_SIZE_4_64BIT		0x00000004
#define PORT_SERIAL_CONTROL		0x003c
#define  SET_MII_SPEED_TO_100		0x01000000
#define  SET_GMII_SPEED_TO_1000		0x00800000
#define  SET_FULL_DUPLEX_MODE		0x00200000
#define  MAX_RX_PACKET_9700BYTE		0x000a0000
#define  DISABLE_AUTO_NEG_SPEED_GMII	0x00002000
#define  DO_NOT_FORCE_LINK_FAIL		0x00000400
#define  SERIAL_PORT_CONTROL_RESERVED	0x00000200
#define  DISABLE_AUTO_NEG_FOR_FLOW_CTRL	0x00000008
#define  DISABLE_AUTO_NEG_FOR_DUPLEX	0x00000004
#define  FORCE_LINK_PASS		0x00000002
#define  SERIAL_PORT_ENABLE		0x00000001
#define PORT_VPT2P			0x0040
#define PORT_STATUS			0x0044
#define  TX_FIFO_EMPTY			0x00000400
#define  TX_IN_PROGRESS			0x00000080
#define  PORT_SPEED_MASK		0x00000030
#define  PORT_SPEED_1000		0x00000010
#define  PORT_SPEED_100			0x00000020
#define  PORT_SPEED_10			0x00000000
#define  FLOW_CONTROL_ENABLED		0x00000008
#define  FULL_DUPLEX			0x00000004
#define  LINK_UP			0x00000002
#define TXQ_COMMAND			0x0048
#define TXQ_FIX_PRIO_CONF		0x004c
#define PORT_SERIAL_CONTROL1		0x004c
#define  CLK125_BYPASS_EN		0x00000010
#define TX_BW_RATE			0x0050
#define TX_BW_MTU			0x0058
#define TX_BW_BURST			0x005c
#define INT_CAUSE			0x0060
#define  INT_TX_END			0x07f80000
#define  INT_TX_END_0			0x00080000
#define  INT_RX				0x000003fc
#define  INT_RX_0			0x00000004
#define  INT_EXT			0x00000002
#define INT_CAUSE_EXT			0x0064
#define  INT_EXT_LINK_PHY		0x00110000
#define  INT_EXT_TX			0x000000ff
#define   INT_EXT_TX_0			0x00000001
#define INT_MASK			0x0068
#define INT_MASK_EXT			0x006c
#define TX_FIFO_URGENT_THRESHOLD	0x0074
#define RX_DISCARD_FRAME_CNT		0x0084
#define RX_OVERRUN_FRAME_CNT		0x0088
#define TXQ_FIX_PRIO_CONF_MOVED		0x00dc
#define TX_BW_RATE_MOVED		0x00e0
#define TX_BW_MTU_MOVED			0x00e8
#define TX_BW_BURST_MOVED		0x00ec
#define RXQ_CURRENT_DESC_PTR(q)		(0x020c + ((q) << 4))
#define RXQ_COMMAND			0x0280
#define TXQ_CURRENT_DESC_PTR(q)		(0x02c0 + ((q) << 2))
#define TXQ_BW_TOKENS(q)		(0x0300 + ((q) << 4))
#define TXQ_BW_CONF(q)			(0x0304 + ((q) << 4))
#define TXQ_BW_WRR_CONF(q)		(0x0308 + ((q) << 4))

/*
 * Misc per-port registers.
 */
#define MIB_COUNTERS(p)			(0x1000 + ((p) << 7))
#define SPECIAL_MCAST_TABLE(p)		(0x1400 + ((p) << 10))
#define OTHER_MCAST_TABLE(p)		(0x1500 + ((p) << 10))
#define UNICAST_TABLE(p)		(0x1600 + ((p) << 10))


/*
 * SDMA configuration register default value.
 */
#if defined(__BIG_ENDIAN)
#define PORT_SDMA_CONFIG_DEFAULT_VALUE		\
		(RX_BURST_SIZE_4_64BIT	|	\
		 TX_BURST_SIZE_4_64BIT)
#elif defined(__LITTLE_ENDIAN)
#define PORT_SDMA_CONFIG_DEFAULT_VALUE		\
		(RX_BURST_SIZE_4_64BIT	|	\
		 BLM_RX_NO_SWAP		|	\
		 BLM_TX_NO_SWAP		|	\
		 TX_BURST_SIZE_4_64BIT)
#else
#error One of __BIG_ENDIAN or __LITTLE_ENDIAN must be defined
#endif


/*
 * Misc definitions.
 */
#define DEFAULT_RX_QUEUE_SIZE	128
#define DEFAULT_TX_QUEUE_SIZE	512
#define RX_OFFSET		ALIGN(NET_SKB_PAD, SMP_CACHE_BYTES)
#define TSO_HEADER_SIZE		128
#define COPY_BREAK_SIZE		128

/* Max number of allowed TCP segments for software TSO */
#define MV643XX_MAX_TSO_SEGS 100
#define MV643XX_MAX_SKB_DESCS (MV643XX_MAX_TSO_SEGS * 2 + MAX_SKB_FRAGS)

#define IS_TSO_HEADER(txq, addr) \
	((addr >= txq->tso_hdrs_dma) && \
	 (addr < txq->tso_hdrs_dma + txq->tx_ring_size * TSO_HEADER_SIZE))

#define DESC_DMA_MAP_SINGLE 0
#define DESC_DMA_MAP_PAGE 1

/*
 * RX/TX descriptors.
 */
#if defined(__BIG_ENDIAN)
struct rx_desc {
	u16 byte_cnt;		/* Descriptor buffer byte count		*/
	u16 buf_size;		/* Buffer size				*/
	u32 cmd_sts;		/* Descriptor command status		*/
	u32 next_desc_ptr;	/* Next descriptor pointer		*/
	u32 buf_ptr;		/* Descriptor buffer pointer		*/
	u32 cookie;
	u32 pad[3];
};

struct tx_desc {
	u16 byte_cnt;		/* buffer byte count			*/
	u16 l4i_chk;		/* CPU provided TCP checksum		*/
	u32 cmd_sts;		/* Command/status field			*/
	u32 next_desc_ptr;	/* Pointer to next descriptor		*/
	u32 buf_ptr;		/* pointer to buffer for this descriptor*/
	u32 cookie;
	u32 cookie_size;
	u32 pad[2];
};
#elif defined(__LITTLE_ENDIAN)
struct rx_desc {
	u32 cmd_sts;		/* Descriptor command status		*/
	u16 buf_size;		/* Buffer size				*/
	u16 byte_cnt;		/* Descriptor buffer byte count		*/
	u32 buf_ptr;		/* Descriptor buffer pointer		*/
	u32 next_desc_ptr;	/* Next descriptor pointer		*/
	u32 cookie;
	u32 pad[3];
};

struct tx_desc {
	u32 cmd_sts;		/* Command/status field			*/
	u16 l4i_chk;		/* CPU provided TCP checksum		*/
	u16 byte_cnt;		/* buffer byte count			*/
	u32 buf_ptr;		/* pointer to buffer for this descriptor*/
	u32 next_desc_ptr;	/* Pointer to next descriptor		*/
	u32 cookie;
	u32 cookie_size;
	u32 pad[2];
};
#else
#error One of __BIG_ENDIAN or __LITTLE_ENDIAN must be defined
#endif

/* RX & TX descriptor command */
#define BUFFER_OWNED_BY_DMA		0x80000000

/* RX & TX descriptor status */
#define ERROR_SUMMARY			0x00000001
#define ERROR_CODE_RX_CRC		(0x0 << 1)
#define ERROR_CODE_RX_OVERRUN		(0x1 << 1)
#define ERROR_CODE_RX_MAX_LENGTH	(0x2 << 1)
#define ERROR_CODE_RX_RESOURCE		(0x3 << 1)
#define ERROR_CODE_MASK			(0x3 << 1)

/* RX descriptor status */
#define LAYER_4_CHECKSUM_OK		0x40000000
#define RX_ENABLE_INTERRUPT		0x20000000
#define RX_FIRST_DESC			0x08000000
#define RX_LAST_DESC			0x04000000
#define RX_IP_HDR_OK			0x02000000
#define RX_PKT_IS_IPV4			0x01000000
#define RX_PKT_IS_ETHERNETV2		0x00800000
#define RX_PKT_LAYER4_TYPE_MASK		0x00600000
#define RX_PKT_LAYER4_TYPE_TCP_IPV4	0x00000000
#define RX_PKT_LAYER4_TYPE_UDP_IPV4	0x00200000
#define RX_PKT_IS_VLAN_TAGGED		0x00080000

/* TX descriptor command */
#define TX_ENABLE_INTERRUPT		0x00800000
#define GEN_CRC				0x00400000
#define TX_FIRST_DESC			0x00200000
#define TX_LAST_DESC			0x00100000
#define ZERO_PADDING			0x00080000
#define GEN_IP_V4_CHECKSUM		0x00040000
#define GEN_TCP_UDP_CHECKSUM		0x00020000
#define UDP_FRAME			0x00010000
#define MAC_HDR_EXTRA_4_BYTES		0x00008000
#define GEN_TCP_UDP_CHK_FULL		0x00000400
#define MAC_HDR_EXTRA_8_BYTES		0x00000200

#define TX_IHL_SHIFT			11


/* global *******************************************************************/
struct mv643xx_eth_shared_private {
	/*
	 * Ethernet controller base address.
	 */
	void __iomem *base;

	/*
	 * Per-port MBUS window access register value.
	 */
	u32 win_protect;

	/*
	 * Hardware-specific parameters.
	 */
	int extended_rx_coal_limit;
	int tx_bw_control;
	int tx_csum_limit;
	int unit;
	struct clk *clk;
};

#define TX_BW_CONTROL_ABSENT		0
#define TX_BW_CONTROL_OLD_LAYOUT	1
#define TX_BW_CONTROL_NEW_LAYOUT	2

static int mv643xx_eth_open(struct net_device *dev);
static int mv643xx_eth_stop(struct net_device *dev);

static int mii_bus_read(struct net_device *dev, int mii_id, int regnum);
static void mii_bus_write(struct net_device *dev, int mii_id, int regnum,
			  int value);


/* per-port *****************************************************************/
struct mib_counters {
	u64 good_octets_received;
	u32 bad_octets_received;
	u32 internal_mac_transmit_err;
	u32 good_frames_received;
	u32 bad_frames_received;
	u32 broadcast_frames_received;
	u32 multicast_frames_received;
	u32 frames_64_octets;
	u32 frames_65_to_127_octets;
	u32 frames_128_to_255_octets;
	u32 frames_256_to_511_octets;
	u32 frames_512_to_1023_octets;
	u32 frames_1024_to_max_octets;
	u64 good_octets_sent;
	u32 good_frames_sent;
	u32 excessive_collision;
	u32 multicast_frames_sent;
	u32 broadcast_frames_sent;
	u32 unrec_mac_control_received;
	u32 fc_sent;
	u32 good_fc_received;
	u32 bad_fc_received;
	u32 undersize_received;
	u32 fragments_received;
	u32 oversize_received;
	u32 jabber_received;
	u32 mac_receive_error;
	u32 bad_crc_event;
	u32 collision;
	u32 late_collision;
	/* Non MIB hardware counters */
	u32 rx_discard;
	u32 rx_overrun;
	/* Non MIB software counters */
	u32 rx_packets_q[8];
	u32 tx_packets_q[8];
};

struct rx_queue {
	int index;

	unsigned int rx_ring_size;
	unsigned int rx_curr_desc;
	unsigned int rx_packets;

	struct rx_desc *rx_desc_area;
	dma_addr_t rx_desc_dma;
	int rx_desc_area_size;
};

#ifdef CONFIG_MV643XX_ETH_FBX_FF
#define NAPI_TX_OFFSET	1
#else
#define NAPI_TX_OFFSET	0
#endif

struct tx_queue {
	int index;

	int tx_ring_size;

	int tx_desc_count;
	int tx_curr_desc;
	int tx_used_desc;

	int tx_stop_threshold;
	int tx_wake_threshold;

	char *tso_hdrs;
	dma_addr_t tso_hdrs_dma;

	struct tx_desc *tx_desc_area;
	char *tx_desc_mapping; /* array to track the type of the dma mapping */
	dma_addr_t tx_desc_dma;
	int tx_desc_area_size;

	struct sk_buff_head tx_skb;

	unsigned long tx_packets;
	unsigned long tx_bytes;
	unsigned long tx_dropped;
};

struct mv643xx_eth_private {
	struct mv643xx_eth_shared_private *shared;
	void __iomem *base;
	int port_num;

	struct net_device *dev;

	struct phy_device *phy;

	struct timer_list mib_counters_timer;
	spinlock_t mib_counters_lock;
	struct mib_counters mib_counters;

	struct work_struct tx_timeout_task;

	struct napi_struct napi;
	u32 int_mask;
	u8 work_link;
	u8 work_tx;
	u8 work_tx_end;
	u8 work_rx;

	unsigned int pkt_size;
	unsigned int frag_size;

	/*
	 * RX state.
	 */
	int rx_ring_size;
	unsigned long rx_desc_sram_addr;
	int rx_desc_sram_size;
	int rxq_count;
	struct rx_queue rxq[8];

	/*
	 * TX state.
	 */
	int tx_ring_size;
	unsigned long tx_desc_sram_addr;
	int tx_desc_sram_size;
	int txq_count;
	struct tx_queue txq[8];

	/*
	 * Hardware-specific parameters.
	 */
	struct clk *clk;
	unsigned int t_clk;

	/*
	 * mii bus for MII ioctls & low level early switch config.
	 */
	struct mii_bus *mii_bus;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	struct tx_queue *ff_txq;
	struct notifier_block ff_notifier;
#endif
};

static struct mv643xx_eth_private *mp_by_unit[4];

/* port register accessors **************************************************/
static inline u32 rdl(struct mv643xx_eth_private *mp, int offset)
{
	return readl(mp->shared->base + offset);
}

static inline u32 rdlp(struct mv643xx_eth_private *mp, int offset)
{
	return readl(mp->base + offset);
}

static inline void wrl(struct mv643xx_eth_private *mp, int offset, u32 data)
{
	writel(data, mp->shared->base + offset);
}

static inline void wrlp(struct mv643xx_eth_private *mp, int offset, u32 data)
{
	writel(data, mp->base + offset);
}


/* rxq/txq helper functions *************************************************/
static struct mv643xx_eth_private *rxq_to_mp(struct rx_queue *rxq)
{
	return container_of(rxq, struct mv643xx_eth_private, rxq[rxq->index]);
}

static struct mv643xx_eth_private *txq_to_mp(struct tx_queue *txq)
{
	return container_of(txq, struct mv643xx_eth_private, txq[txq->index]);
}

static void rxq_enable(struct rx_queue *rxq)
{
	struct mv643xx_eth_private *mp = rxq_to_mp(rxq);
	wrlp(mp, RXQ_COMMAND, 1 << rxq->index);
}

static void rxq_disable(struct rx_queue *rxq)
{
	struct mv643xx_eth_private *mp = rxq_to_mp(rxq);
	u8 mask = 1 << rxq->index;

	wrlp(mp, RXQ_COMMAND, mask << 8);
	while (rdlp(mp, RXQ_COMMAND) & mask)
		udelay(10);
}

static void txq_reset_hw_ptr(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	u32 addr;

	addr = (u32)txq->tx_desc_dma;
	addr += txq->tx_curr_desc * sizeof(struct tx_desc);
	wrlp(mp, TXQ_CURRENT_DESC_PTR(txq->index), addr);
}

static void txq_enable(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	wrlp(mp, TXQ_COMMAND, 1 << txq->index);
}

static void txq_disable(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	u8 mask = 1 << txq->index;

	wrlp(mp, TXQ_COMMAND, mask << 8);
	while (rdlp(mp, TXQ_COMMAND) & mask)
		udelay(10);
}

static void txq_maybe_wake(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	struct netdev_queue *nq;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	WARN_ON(txq->index == 0);
#endif
	nq = netdev_get_tx_queue(mp->dev, txq->index - NAPI_TX_OFFSET);

	if (netif_tx_queue_stopped(nq)) {
		__netif_tx_lock(nq, smp_processor_id());
		if (txq->tx_desc_count <= txq->tx_wake_threshold)
			netif_tx_wake_queue(nq);
		__netif_tx_unlock(nq);
	}
}

static void *mv643xx_eth_frag_alloc(const struct mv643xx_eth_private *mp)
{
	if (likely(mp->frag_size <= PAGE_SIZE))
		return napi_alloc_frag(mp->frag_size);
	else
		return kmalloc(mp->frag_size, GFP_ATOMIC);
}

static void mv643xx_eth_frag_free(const struct mv643xx_eth_private *mp,
				  void *data)
{
	if (likely(mp->frag_size <= PAGE_SIZE))
		skb_free_frag(data);
	else
		kfree(data);
}

static inline bool pkt_is_ipv4(u32 cmd_sts)
{
	return (cmd_sts & RX_PKT_IS_IPV4) == RX_PKT_IS_IPV4;
}

static inline bool pkt_is_vlan(u32 cmd_sts)
{
	return (cmd_sts & RX_PKT_IS_VLAN_TAGGED) == RX_PKT_IS_VLAN_TAGGED;
}

static inline bool pkt_is_tcp4(u32 cmd_sts)
{
	return (cmd_sts & RX_PKT_LAYER4_TYPE_MASK) ==
		RX_PKT_LAYER4_TYPE_TCP_IPV4;
}

static inline bool pkt_is_udp4(u32 cmd_sts)
{
	return (cmd_sts & RX_PKT_LAYER4_TYPE_MASK) ==
		RX_PKT_LAYER4_TYPE_UDP_IPV4;
}

#ifdef CONFIG_MV643XX_ETH_FBX_FF

static bool ff_enabled;
static unsigned int ff_mode;

static bool ff_tx_queue_can_reclaim(struct tx_queue *txq)
{
	struct tx_desc *desc;
	int tx_index;

	if (!txq || !txq->tx_desc_count)
		return false;

	tx_index = txq->tx_used_desc;
	desc = &txq->tx_desc_area[tx_index];

	if ((desc->cmd_sts & BUFFER_OWNED_BY_DMA))
		return false;

	return true;
}


		/*
 * size on which we invalidate data when we reclaim fast-forwarded
 * buffer
		 *
 * worst case read lookup by rx path from RX_OFFSET is (VLAN + ip6 +
 * iph + tcphdr)
		 */
#define FF_MAP_SIZE	(sizeof (struct vlan_ethhdr) + \
			 sizeof (struct ipv6hdr) + \
			 sizeof (struct iphdr) + \
			 sizeof (struct tcphdr))

static void *ff_tx_queue_frag_reclaim(struct mv643xx_eth_private *mp,
				      unsigned int needed_frag_size)
{
	struct tx_queue *txq = mp->ff_txq;
	struct tx_desc *desc;
	void *frag;
	unsigned int frag_size;
	int tx_index;

	if (!txq || !txq->tx_desc_count)
		return NULL;

	tx_index = txq->tx_used_desc;
	desc = &txq->tx_desc_area[tx_index];

	if ((desc->cmd_sts & BUFFER_OWNED_BY_DMA))
		return NULL;

	txq->tx_used_desc = tx_index + 1;
	if (txq->tx_used_desc == txq->tx_ring_size)
		txq->tx_used_desc = 0;

	txq->tx_desc_count--;

	frag = (void *)desc->cookie;
	frag_size = desc->cookie_size;

	if (frag_size != needed_frag_size) {
		skb_free_frag(frag);
		return NULL;
	}

	return frag;
}
#endif

static int rx_desc_refill(struct mv643xx_eth_private *mp,
			  struct rx_desc *rx_desc, bool unmap)
{
	unsigned int map_size = mp->pkt_size;
	void *frag;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	if (ff_enabled) {
		struct mv643xx_eth_private *omp;

		/*
		 * try to reclaim from "opposite" fast forward dedicated tx
		 * queue
		 */
		if (ff_mode == 1)
			omp = mp_by_unit[1 - mp->shared->unit];
		else
			omp = mp;

		if (omp)
			frag = ff_tx_queue_frag_reclaim(omp, mp->pkt_size);
		else
			frag = NULL;

		if (!frag)
			frag = mv643xx_eth_frag_alloc(mp);
		else
			map_size = FF_MAP_SIZE;
	} else
#endif
		frag = mv643xx_eth_frag_alloc(mp);

	if (!frag)
		return -ENOMEM;

	if (unmap)
		dma_unmap_single(mp->dev->dev.parent, rx_desc->buf_ptr,
				 mp->pkt_size, DMA_FROM_DEVICE);

	rx_desc->buf_ptr = dma_map_single(mp->dev->dev.parent,
					  frag + RX_OFFSET,
					  map_size,
					  DMA_FROM_DEVICE);
	rx_desc->buf_size = mp->pkt_size;
	rx_desc->cookie = (u32)frag;
	wmb();
	rx_desc->cmd_sts = BUFFER_OWNED_BY_DMA | RX_ENABLE_INTERRUPT;
	wmb();
	return 0;
}

#ifdef CONFIG_MV643XX_ETH_FBX_FF

static struct {
	struct net_device	*wan_dev;
	struct net_device	*lan_dev;
	unsigned long		jiffies;

	struct net_device	*tun_dev;
	u8			tun_ready:1;
	u16			tun_mtu;

	/* sit parameters */
	union ff_tun_params {
		struct {
			u32		src;
			u32		s6rd_prefix;
			u32		s6rd_pmask;
			u8		s6rd_plen;
		} sit;

		struct {
			/* map parameters */
			u32		ipv4_prefix;
			u32		ipv4_pmask;
			u8		ipv4_plen;
			u8		ipv6_plen;
			struct in6_addr	src;
			struct in6_addr	br;

			u64		ipv6_prefix;
			u32		ea_addr_mask;
			u16		ea_port_mask;
			u8		psid_len;
			u8		ea_lshift;
		} map;
	} u;

	char			tun_dev_name[IFNAMSIZ];
} ff;

static LIST_HEAD(ff_devs);

struct ff_dev {
	const char		*desc;
	unsigned int		unit;
	bool			bridge_member;
	unsigned int		vlan;
	struct net_device	**pvirt_dev;

	bool			active;
	struct net_bridge_port	*br_port;
	bool			dev_up;
	struct list_head	next;
};

static inline bool is_bridge_dev(struct net_device *dev)
{
        return dev->priv_flags & IFF_EBRIDGE;
}

static u32 gen_netmask(u8 len)
{
	return htonl(~((1 << (32 - len)) - 1));
}

static void __ff_tun_set_params(bool ready,
				unsigned int mtu,
				const union ff_tun_params *tp)
{
	if (!ready) {
		if (!ff.tun_ready)
			return;

		printk(KERN_DEBUG "ff: tunnel now NOT ready\n");
		ff.tun_ready = 0;
		return;
	}

	if (ff.tun_ready) {
		if (ff.tun_mtu == mtu && !memcmp(tp, &ff.u, sizeof (*tp)))
			return;
	}

	ff.tun_mtu = mtu;
	memcpy(&ff.u, tp, sizeof (*tp));

	if (!ff.tun_ready)
		printk(KERN_DEBUG "ff: tunnel now ready\n");
	else
		printk(KERN_DEBUG "ff: tunnel params updated\n");

	ff.tun_ready = true;
}

static void __ff_tun_read_params(void)
{
	union ff_tun_params tp;

	if (!ff.tun_dev)
		return;

	if (!ff.wan_dev) {
		__ff_tun_set_params(false, 0, NULL);
		return;
	}

	memset(&tp, 0, sizeof (tp));

	if (ff.tun_dev->type == ARPHRD_SIT) {
		const struct ip_tunnel *tun = netdev_priv(ff.tun_dev);
		const struct ip_tunnel_6rd_parm *ip6rd = &tun->ip6rd;

		if (!ip6rd->prefixlen || ip6rd->prefixlen > 32) {
			printk(KERN_DEBUG "ff: unsupported 6rd plen\n");
			__ff_tun_set_params(false, 0, NULL);
			return;
		}

		if (ff.tun_dev->mtu + sizeof (struct iphdr) >
		    ff.wan_dev->mtu) {
			printk(KERN_DEBUG "ff: WAN mtu too "
			       "small for tunnel (%u => %u)\n",
			       ff.tun_dev->mtu, ff.wan_dev->mtu);
			__ff_tun_set_params(false, 0, NULL);
			return;
		}

		tp.sit.src = tun->parms.iph.saddr;
		tp.sit.s6rd_prefix = ip6rd->prefix.s6_addr32[0];
		tp.sit.s6rd_pmask = gen_netmask(ip6rd->prefixlen);
		tp.sit.s6rd_plen = ip6rd->prefixlen;
		__ff_tun_set_params(true, ff.tun_dev->mtu, &tp);
		return;
	}

	if (ff.tun_dev->type == ARPHRD_TUNNEL6) {
		const struct ip6_tnl *t = netdev_priv(ff.tun_dev);
		const struct __ip6_tnl_parm *prm = &t->parms;
		const struct __ip6_tnl_fmr *fmr;

		if (ff.tun_dev->mtu + sizeof (struct ipv6hdr) >
		    ff.wan_dev->mtu) {
			printk(KERN_DEBUG "ff: WAN mtu too "
			       "small for tunnel (%u => %u)\n",
			       ff.tun_dev->mtu, ff.wan_dev->mtu);
			__ff_tun_set_params(false, 0, NULL);
			return;
		}

		tp.map.src = prm->laddr;
		tp.map.br = prm->raddr;

		fmr = prm->fmrs;
		if (!fmr) {
			tp.map.ipv4_prefix = 0;
			__ff_tun_set_params(true, ff.tun_dev->mtu, &tp);
			return;
		}

		if (fmr->ip6_prefix_len < 32 ||
		    (fmr->ip6_prefix_len + 32 - fmr->ip4_prefix_len > 64)) {
			printk(KERN_DEBUG "ff: unsupp MAP-E: eabits "
			       "span 32 bits\n");
			__ff_tun_set_params(false, 0, NULL);
			return;
		}

		if (fmr->offset) {
			printk(KERN_DEBUG "ff: unsupp MAP-E: non zero "
			       "PSID offset\n");
			__ff_tun_set_params(false, 0, NULL);
			return;
		}

		tp.map.ipv4_prefix = fmr->ip4_prefix.s_addr;
		tp.map.ipv4_pmask = gen_netmask(fmr->ip4_prefix_len);
		tp.map.ipv4_plen = fmr->ip4_prefix_len;
		tp.map.ipv6_plen = fmr->ip6_prefix_len;
		memcpy(&tp.map.ipv6_prefix, &fmr->ip6_prefix, 8);

		tp.map.ea_addr_mask = ~gen_netmask(fmr->ip4_prefix_len);
		if (fmr->ea_len <= 32 - fmr->ip4_prefix_len) {
			/* v4 prefix or full IP */
			u32 addr_bits;

			addr_bits = fmr->ip4_prefix_len + fmr->ea_len;
			if (addr_bits != 32)
				tp.map.ea_addr_mask &= gen_netmask(addr_bits);
			tp.map.psid_len = 0;
		} else {
			u8 psid_len;

			psid_len = fmr->ea_len - (32 - fmr->ip4_prefix_len);
			tp.map.psid_len = psid_len;
			tp.map.ea_port_mask = gen_netmask(psid_len);
		}

		tp.map.ea_lshift = 32 - (fmr->ip6_prefix_len - 32) -
			fmr->ea_len;

		__ff_tun_set_params(true, ff.tun_dev->mtu, &tp);
		return;
	}
}

static void ff_tun_capture(void)
{
	struct net_device *dev;

	local_bh_disable();
	if (ff.tun_dev) {
		local_bh_enable();
		printk(KERN_ERR "ff: error: tun already registered\n");
		return;
	}

	dev = dev_get_by_name(&init_net, ff.tun_dev_name);
	if (!dev) {
		local_bh_enable();
		return;
	}

	if (dev->type != ARPHRD_SIT && dev->type != ARPHRD_TUNNEL6) {
		local_bh_enable();
		return;
	}

	if (!(dev->flags & IFF_UP)) {
		dev_put(ff.tun_dev);
		local_bh_enable();
		return;
	}

	ff.tun_dev = dev;
	__ff_tun_read_params();
	local_bh_enable();
	printk(KERN_INFO "ff: tun dev grabbed\n");
}

static void ff_tun_release(void)
{
	local_bh_disable();
	dev_put(ff.tun_dev);
	ff.tun_dev = NULL;
	local_bh_enable();
	printk(KERN_INFO "ff: tun dev released\n");
}

static int ff_device_event(struct notifier_block *this,
			   unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct mv643xx_eth_private *mp;
	struct ff_dev *ff_dev;

	if (!net_eq(dev_net(dev), &init_net))
		return 0;

	if (!strcmp(dev->name, ff.tun_dev_name)) {
		local_bh_disable();

		switch (event) {
		case NETDEV_UP:
			if (!ff.tun_dev)
				ff_tun_capture();
			break;

		case NETDEV_CHANGE:
		case NETDEV_CHANGEMTU:
			if (ff.tun_dev == dev)
				__ff_tun_read_params();
			break;

		case NETDEV_GOING_DOWN:
		case NETDEV_DOWN:
		case NETDEV_UNREGISTER:
			if (ff.tun_dev == dev)
				ff_tun_release();
			break;
		}

		local_bh_enable();
		return 0;
	}

	list_for_each_entry(ff_dev, &ff_devs, next) {
		mp = container_of(this, typeof(*mp), ff_notifier);
		if (mp->shared->unit != ff_dev->unit)
			continue;

		if (ff_dev->vlan) {
			if (!is_vlan_dev(dev))
				continue;

			switch (event) {
			case NETDEV_UP:
				if (vlan_dev_upper_dev(dev) != mp->dev ||
				    vlan_dev_vlan_id(dev) != ff_dev->vlan)
					continue;

				if (ff_dev->active)
					continue;

				dev_hold(dev);

				local_bh_disable();
				*(ff_dev->pvirt_dev) = dev;

				if (ff_dev->pvirt_dev == &ff.wan_dev)
					__ff_tun_read_params();
				local_bh_enable();

				ff_dev->active = true;
				printk(KERN_INFO "ff: ff_dev %s: active "
				       "for %s\n", ff_dev->desc,
				       dev->name);
				break;

			case NETDEV_GOING_DOWN:
			case NETDEV_DOWN:
			case NETDEV_UNREGISTER:
				if (!ff_dev->active)
					continue;

				if (vlan_dev_upper_dev(dev) != mp->dev ||
				    vlan_dev_vlan_id(dev) != ff_dev->vlan)
					continue;

				local_bh_disable();
				*(ff_dev->pvirt_dev) = NULL;
				local_bh_enable();
				dev_put(dev);
				ff_dev->active = false;

				printk(KERN_INFO "ff: ff_dev %s: now "
				       "inactive\n", ff_dev->desc);
				break;

			default:
				break;
			}
		}

		if (ff_dev->bridge_member) {
			struct net_bridge *br;
			bool ok;

			switch (event) {
			case NETDEV_UP:
				if (dev == mp->dev)
					ff_dev->dev_up = true;
				break;

			case NETDEV_GOING_DOWN:
			case NETDEV_DOWN:
			case NETDEV_UNREGISTER:
				if (dev == mp->dev)
					ff_dev->dev_up = false;
				break;

			case NETDEV_CHANGEUPPER:
				if (dev == mp->dev) {
					if ((dev->priv_flags &
					     IFF_BRIDGE_PORT) &&
					    netdev_master_upper_dev_get(dev))
						ff_dev->br_port = br_port_get_rcu(dev);
					else
						ff_dev->br_port = NULL;
				}
				break;

			default:
				break;
			}

			ok = false;
			if (ff_dev->dev_up && ff_dev->br_port) {
				br = ff_dev->br_port->br;
				if (br->dev->flags & IFF_UP)
					ok = true;
			}

			if (!(ok ^ ff_dev->active))
				continue;

			if (ok) {
				dev_hold(br->dev);
				local_bh_disable();
				*(ff_dev->pvirt_dev) = br->dev;
				local_bh_enable();
				ff_dev->active = true;

				printk(KERN_INFO "ff: ff_dev %s: active "
				       "for %s\n", ff_dev->desc,
				       br->dev->name);


			} else {
				dev = *(ff_dev->pvirt_dev);
				local_bh_disable();
				*(ff_dev->pvirt_dev) = NULL;
				local_bh_enable();
				dev_put(dev);
				ff_dev->active = false;
				printk(KERN_INFO "ff: ff_dev %s: "
				       "now inactive\n", ff_dev->desc);
			}
		}
	}

	return 0;
}

enum ff_xmit_mode {
	FF_XMIT_IPV4,
	FF_XMIT_IPV6,
	FF_XMIT_IPV6_IN_IPV4,
	FF_XMIT_IPV4_IN_IPV6,
};

		/*
 *
		 */
static bool ff_send(struct tx_queue *txq,
		    u32 dma_buf_addr,
		    void *frag,
		    u32 frag_size,
		    unsigned int send_len,
		    unsigned int clean_len,
		    bool hw_l3_checksum,
		    bool hw_l4_checksum,
		    bool is_tcp,
		    bool is_vlan)
{
	struct tx_desc *tx_desc;
	unsigned int tx_index;
	u32 cmd_sts;

	if (WARN_ON(txq->tx_desc_count == txq->tx_ring_size))
		return 1;

	dma_sync_single_for_device(NULL, dma_buf_addr,
				   clean_len, DMA_TO_DEVICE);

	tx_index = txq->tx_curr_desc++;
	if (txq->tx_curr_desc == txq->tx_ring_size)
		txq->tx_curr_desc = 0;

	txq->tx_desc_count++;
	txq->tx_desc_mapping[tx_index] = DESC_DMA_MAP_SINGLE;

	tx_desc = &txq->tx_desc_area[tx_index];
	tx_desc->byte_cnt = send_len;
	tx_desc->buf_ptr = dma_buf_addr;
	tx_desc->cookie = (u32)frag;
	tx_desc->cookie_size = (u32)frag_size;

	cmd_sts = TX_FIRST_DESC |
		TX_LAST_DESC |
		GEN_CRC |
		BUFFER_OWNED_BY_DMA |
		ZERO_PADDING;

	if (hw_l3_checksum) {
		cmd_sts |= (GEN_IP_V4_CHECKSUM |
			    (5 << TX_IHL_SHIFT));

		if (is_vlan)
			cmd_sts |= MAC_HDR_EXTRA_4_BYTES;
	}

	if (hw_l4_checksum) {
		cmd_sts |= (GEN_TCP_UDP_CHECKSUM |
			    GEN_TCP_UDP_CHK_FULL);

		if (!is_tcp)
			cmd_sts |= UDP_FRAME;
	}

	tx_desc->cmd_sts = cmd_sts;
	txq_enable(txq);

	txq->tx_bytes += send_len;
	txq->tx_packets++;

	return 0;
}

/*
 * IP_FFN private data
 */
struct ffn_priv {
	struct in6_addr		tun_dest_ip6;
	struct dst_entry	*tun_dst;
};

static void ffn_priv_release(struct ffn_priv *priv)
{
	dst_release(priv->tun_dst);
}

static void ffn_priv_destructor_cb(void *data)
{
	struct ffn_priv *priv = (struct ffn_priv *)data;
	ffn_priv_release(priv);
}

static struct ffn_priv *ffn_get_ro_priv(struct ffn_lookup_entry *e)
{
	if (e->manip.priv_destructor != ffn_priv_destructor_cb)
		return NULL;

	return (struct ffn_priv *)e->manip.ffn_priv_area;
}

static struct ffn_priv *ffn_get_rw_priv(struct ffn_lookup_entry *e)
{
	BUILD_BUG_ON(sizeof (e->manip.ffn_priv_area) <
		     sizeof (struct ffn_priv));

	if (e->manip.priv_destructor &&
	    e->manip.priv_destructor != ffn_priv_destructor_cb)
		return NULL;

	return (struct ffn_priv *)e->manip.ffn_priv_area;
}

/*
 * IPV6_FFN private data
 */
struct ffn6_priv {
	u32			tun_dest_ip;
	struct dst_entry	*tun_dst;
};

static void ffn6_priv_release(struct ffn6_priv *priv)
{
	dst_release(priv->tun_dst);
}

static void ffn6_priv_destructor_cb(void *data)
{
	struct ffn6_priv *priv = (struct ffn6_priv *)data;
	ffn6_priv_release(priv);
}

static struct ffn6_priv *ffn6_get_ro_priv(struct ffn6_lookup_entry *e6)
{
	if (e6->manip.priv_destructor != ffn6_priv_destructor_cb)
		return NULL;

	return (struct ffn6_priv *)e6->manip.ffn_priv_area;
}

static struct ffn6_priv *ffn6_get_rw_priv(struct ffn6_lookup_entry *e6)
{
	BUILD_BUG_ON(sizeof (e6->manip.ffn_priv_area) <
		     sizeof (struct ffn6_priv));

	if (e6->manip.priv_destructor &&
	    e6->manip.priv_destructor != ffn6_priv_destructor_cb)
		return NULL;

	return (struct ffn6_priv *)e6->manip.ffn_priv_area;
}

/*
 *
 */
static u32 ff_tun_extract_6rd_addr(const struct in6_addr *d)
{
	u32 a1, a2;

	a1 = ntohl(d->s6_addr32[0] & ~ff.u.sit.s6rd_pmask);
	a1 <<= ff.u.sit.s6rd_plen;

	a2 = ntohl(d->s6_addr32[1] & ff.u.sit.s6rd_pmask);
	a2 >>= (32 - ff.u.sit.s6rd_plen);
	return htonl(a1 | a2);
}

/*
 *
 */
static void ff_tun_gen_mape_addr(u32 addr, u16 port, struct in6_addr *dest)
{
	u32 eabits;
	u16 psid;

	eabits = ntohl(addr & ff.u.map.ea_addr_mask) << ff.u.map.psid_len;
	psid = 0;
	if (ff.u.map.psid_len) {
		psid = ntohs(port & ff.u.map.ea_port_mask) >>
			(16 - ff.u.map.psid_len);
		eabits |= psid;
	}

	memcpy(dest, &ff.u.map.ipv6_prefix, 8);
	dest->s6_addr32[1] |= htonl(eabits << ff.u.map.ea_lshift);

	dest->s6_addr32[2] = htonl(ntohl(addr) >> 16);
	dest->s6_addr32[3] = htonl((ntohl(addr) << 16) | psid);
}

/*
 *
 */
static bool ff_forward(struct mv643xx_eth_private *rx_mp,
		       struct mv643xx_eth_private *tx_mp,
		       struct net_device *rx_dev,
		       struct net_device *tx_dev,
		       unsigned int rx_vlan,
		       unsigned int tx_vlan,
		       struct rx_desc *rx_desc,
		       unsigned int cmd_sts,
		       void *frag,
		       size_t offset, size_t eth_len)
{
	struct net_device_stats *rx_hw_stats;
	struct net_device_stats *tx_hw_stats;
	struct ffn_lookup_entry *e;
	struct ffn6_lookup_entry *e6;
	struct tx_queue *txq;
	struct neighbour *neigh;
	struct nf_conn *ct;
	struct ethhdr *eth;
	enum ff_xmit_mode xmit_mode;
	u32 buf_addr;
	unsigned int timeout;
	void *l3_hdr, *l4_hdr;
	bool l3_is_ipv4, l4_is_tcp;
	unsigned int l3_plen;
	unsigned int clean_len;
	u32 tun_v4_dest;
	const struct in6_addr *tun_v6_pdest;
	u16 proto;

	/* make sure we have headroom for the worst case scenario */
	BUILD_BUG_ON(CONFIG_NETSKBPAD <
		     (sizeof (struct ipv6hdr) + VLAN_HLEN));

	if (!tx_mp || !rx_dev || !tx_dev)
		return false;

	/* hardware skip 2 bytes to align IP header */
	eth = (struct ethhdr *)((uint8_t *)frag + offset + 2);
	eth_len -= 2;

	/*
	 * filter only IPv4 & IPv6 packets
	 */
	if (rx_vlan) {
		struct vlan_ethhdr *vhdr;

		if (!pkt_is_vlan(cmd_sts))
			return false;

		vhdr = (struct vlan_ethhdr *)eth;
		if (vhdr->h_vlan_TCI != htons(rx_vlan))
			return false;

		if (!pkt_is_ipv4(cmd_sts)) {
			if (vhdr->h_vlan_encapsulated_proto !=
			    htons(ETH_P_IPV6))
				return false;
		}

		l3_hdr = vhdr + 1;
		l3_plen = eth_len - VLAN_ETH_HLEN;
	} else {
		if (pkt_is_vlan(cmd_sts))
			return false;

		if (!pkt_is_ipv4(cmd_sts)) {
			if (eth->h_proto != htons(ETH_P_IPV6))
				return false;
		}

		l3_hdr = eth + 1;
		l3_plen = eth_len - ETH_HLEN;
	}

	/* make sure packet is for our mac address */
	if (memcmp(eth->h_dest, rx_mp->dev->dev_addr, 6)) {
		return false;
	}

	l3_is_ipv4 = pkt_is_ipv4(cmd_sts);
	if (l3_is_ipv4) {
		struct iphdr *iph;
		u16 sport, dport;
		u8 ip_proto;

handle_ipv4:
		iph = (struct iphdr *)l3_hdr;

		/* lookup IP ffn entry */
		if (iph->ihl > 5 || (iph->frag_off & htons(IP_MF | IP_OFFSET)))
			return false;

		if (iph->ttl <= 1)
			return false;

		ip_proto = iph->protocol;
		if (ip_proto == IPPROTO_TCP) {
			struct tcphdr *tcph;

			if (l3_plen < sizeof (*iph) + sizeof (*tcph))
				return false;

			tcph = (struct tcphdr *)((u8 *)iph + 20);
			if (tcph->fin ||
			    tcph->syn ||
			    tcph->rst ||
			    !tcph->ack) {
				return false;
			}

			sport = tcph->source;
			dport = tcph->dest;
			l4_hdr = tcph;
			l4_is_tcp = true;

		} else if (ip_proto == IPPROTO_UDP) {
			struct udphdr *udph;

			if (l3_plen < sizeof (*iph) + sizeof (*udph))
				return false;

			udph = (struct udphdr *)((u8 *)iph + 20);
			sport = udph->source;
			dport = udph->dest;
			l4_hdr = udph;
			l4_is_tcp = false;

		} else if (ip_proto == IPPROTO_IPV6) {
			struct ipv6hdr *ip6hdr;
			u32 ip6rd_daddr;

			if (!ff.tun_ready)
				return false;

			/* must be for us */
			if (iph->daddr != ff.u.sit.src)
				return false;

			/* check len */
			if (l3_plen < sizeof (struct iphdr) +
			    sizeof (struct ipv6hdr))
				return false;

			ip6hdr = (struct ipv6hdr *)(iph + 1);

			/* must belong to 6rd prefix */
			if ((ip6hdr->daddr.s6_addr32[0] &
			     ff.u.sit.s6rd_pmask) != ff.u.sit.s6rd_prefix)
				return false;

			/* 6rd address */
			ip6rd_daddr = ff_tun_extract_6rd_addr(&ip6hdr->daddr);
			if (ip6rd_daddr != ff.u.sit.src)
				return false;

			/* TODO: should check for spoofing here */
			l3_hdr = ip6hdr;
			l3_plen -= 20;
			l3_is_ipv4 = false;
			goto handle_ipv6;

		} else
			return false;

		e = __ffn_get(iph->saddr, iph->daddr,
			      sport, dport, l4_is_tcp);
		if (!e)
			return false;

		if (e->manip.dst->obsolete > 0)
			return false;

		ct = e->manip.ct;

		/* only fast forward TCP connections in established state */
		if (l4_is_tcp &&
		    ct->proto.tcp.state != TCP_CONNTRACK_ESTABLISHED)
			return false;

		/* find out if the packet is to be sent as-is or
		 * tunneled */
		if (ff.tun_dev && e->manip.dst->dev == ff.tun_dev) {
			struct ffn_priv *ffn_priv;
			struct dst_entry *v6_dst;
			struct in6_addr *pdest, *nexthop, dest;
			struct rt6_info *rt6;

			/* IPv4 tunneled into MAP-E device */
			if (!ff.tun_ready) {
				return false;
			}

			if (l3_plen > ff.tun_mtu)
				return false;

			/* lookup ipv6 route cache */
			ffn_priv = ffn_get_ro_priv(e);
			if (ffn_priv) {
				if (ffn_priv->tun_dst->obsolete < 0) {
					/* valid route found */
					v6_dst = ffn_priv->tun_dst;
					pdest = &ffn_priv->tun_dest_ip6;
					goto cached_ipv6_route;
				}

				ffn_priv_release(ffn_priv);
				e->manip.priv_destructor = NULL;
			}

			/* cache miss, compute IPv6 destination */
			if ((iph->daddr & ff.u.map.ipv4_pmask) ==
			    ff.u.map.ipv4_prefix) {
				/* compute dest using FMR */
				ff_tun_gen_mape_addr(iph->daddr, dport, &dest);
				pdest = &dest;
			} else {
				/* next hop is BR */
				pdest = &ff.u.map.br;
			}

			/* v6 route lookup */
			rt6 = rt6_lookup(&init_net, pdest, NULL, 0, 0);
			if (!rt6)
				return false;

			ffn_priv = ffn_get_rw_priv(e);
			if (!ffn_priv)
				return false;

			/* cache this inside FFN private area */
			ffn_priv->tun_dst = (struct dst_entry *)rt6;
			memcpy(&ffn_priv->tun_dest_ip6, pdest, 16);
			e->manip.priv_destructor = ffn_priv_destructor_cb;

			v6_dst = (struct dst_entry *)rt6;

cached_ipv6_route:
			if (v6_dst->dev != tx_dev) {
				return false;
			}

			/* is the neighboor ready ? */
			rt6 = (struct rt6_info *)v6_dst;
			nexthop = rt6_nexthop(rt6, pdest);
			if (!nexthop) {
				return false;
			}

			neigh = __ipv6_neigh_lookup_noref(tx_dev, nexthop);
			if (!neigh) {
				return false;
			}

			xmit_mode = FF_XMIT_IPV4_IN_IPV6;
			tun_v6_pdest = &ffn_priv->tun_dest_ip6;

		} else if (e->manip.dst->dev == tx_dev) {
			const struct rtable *rt;
			u32 nexthop;

			/* is the neighboor ready ? */
			rt = (const struct rtable *)e->manip.dst;

			nexthop = (__force u32)rt_nexthop(rt, e->manip.new_dip);
			neigh = __ipv4_neigh_lookup_noref(tx_dev, nexthop);
			if (!neigh) {
				return false;
			}

			xmit_mode = FF_XMIT_IPV4;
		} else
			return false;

	} else {
		struct ipv6hdr *ip6hdr;
		u16 sport, dport;
		u8 ip_proto;

handle_ipv6:
		ip6hdr = (struct ipv6hdr *)l3_hdr;

		if (ip6hdr->hop_limit <= 1 || !ip6hdr->payload_len)
			return false;

		if (ntohs(ip6hdr->payload_len) > l3_plen)
			return false;

		ip_proto = ip6hdr->nexthdr;

		if (ip_proto == IPPROTO_TCP) {
			struct tcphdr *tcph;

			if (l3_plen < sizeof (*ip6hdr) + sizeof (*tcph))
				return false;

			tcph = (struct tcphdr *)((u8 *)ip6hdr +
						 sizeof (*ip6hdr));

			if (tcph->fin ||
			    tcph->syn ||
			    tcph->rst ||
			    !tcph->ack) {
				return false;
			}

			sport = tcph->source;
			dport = tcph->dest;
			l4_hdr = tcph;
			l4_is_tcp = true;

		} else if (ip_proto == IPPROTO_UDP) {
			struct udphdr *udph;

			if (l3_plen < sizeof (*ip6hdr) + sizeof (*udph))
				return false;

			udph = (struct udphdr *)((u8 *)ip6hdr +
						 sizeof (*ip6hdr));
			sport = udph->source;
			dport = udph->dest;
			l4_hdr = udph;
			l4_is_tcp = false;

		} else if (ip_proto == IPPROTO_IPIP) {
			struct iphdr *iph;

			if (!ff.tun_ready)
				return false;

			/* must be for us */
			if (memcmp(&ip6hdr->daddr, &ff.u.map.src, 16))
				return false;

			/* check len */
			if (l3_plen < sizeof (struct iphdr) +
			    sizeof (struct ipv6hdr))
				return false;

			iph = (struct iphdr *)(ip6hdr + 1);

			/* does it come from BR ? */
			if (memcmp(&ip6hdr->saddr, &ff.u.map.br, 16)) {
				struct in6_addr exp_src_addr;

				/* no, check FMR for spoofing */
				if (!ff.u.map.ipv4_prefix)
					return false;

				/* check up to PSID to reduce lookup
				 * depth */
				ff_tun_gen_mape_addr(iph->saddr, 0,
						     &exp_src_addr);
				if (!ipv6_prefix_equal(&ip6hdr->saddr,
						       &exp_src_addr,
						       ff.u.map.ipv6_plen +
						       ff.u.map.ipv4_plen))
					return false;
			}

			l3_hdr = iph;
			l3_plen -= sizeof (*ip6hdr);
			l3_is_ipv4 = true;
			goto handle_ipv4;

		} else
			return false;

		e6 = __ffn6_get(ip6hdr->saddr.s6_addr32,
				ip6hdr->daddr.s6_addr32,
				sport, dport, l4_is_tcp);

		if (!e6) {
			return false;
		}

		if (e6->manip.dst->obsolete > 0) {
			return false;
		}

		ct = e6->manip.ct;

		/* only fast forward TCP connections in established state */
		if (l4_is_tcp &&
		    ct->proto.tcp.state != TCP_CONNTRACK_ESTABLISHED) {
			return false;
		}

		/* find out if the packet is to be sent as-is or
		 * tunneled */
		if (ff.tun_dev && e6->manip.dst->dev == ff.tun_dev) {
			struct ffn6_priv *ffn6_priv;
			struct dst_entry *v4_dst;
			struct flowi4 fl4;
			struct rtable *rt;
			u32 dest, nexthop;

			/* IPv6 tunneled into SIT device using 6rd */
			if (!ff.tun_ready) {
				return false;
			}

			if (l3_plen > ff.tun_mtu)
				return false;

			/* lookup ipv4 route cache */
			ffn6_priv = ffn6_get_ro_priv(e6);
			if (ffn6_priv) {
				if (!ffn6_priv->tun_dst->obsolete) {
					/* valid route found */
					v4_dst = ffn6_priv->tun_dst;
					dest = ffn6_priv->tun_dest_ip;
					goto cached_ipv4_route;
				}

				ffn6_priv_release(ffn6_priv);
				e6->manip.priv_destructor = NULL;
			}

			/* cache miss, compute IPv4 destination */
			if ((ip6hdr->daddr.s6_addr32[0] &
			     ff.u.sit.s6rd_pmask) == ff.u.sit.s6rd_prefix) {
				/* next hop via prefix */
				dest = ff_tun_extract_6rd_addr(&ip6hdr->daddr);
			} else {
				struct in6_addr *nh6;
				struct rt6_info *rt6;

				/* next hop via route */
				rt6 = (struct rt6_info *)e6->manip.dst;
				nh6 = rt6_nexthop(rt6,
				      (struct in6_addr *)e6->manip.new_dip);
				if (!nh6) {
					return false;
				}

				/* should be a v4 mapped */
				if (nh6->s6_addr32[0] != 0 ||
				    nh6->s6_addr32[1] != 0 ||
				    nh6->s6_addr32[2] != 0) {
					return false;
				}

				dest = nh6->s6_addr32[3];
			}

			/* v4 route lookup */
			rt = ip_route_output_ports(&init_net, &fl4, NULL,
						   dest, ff.u.sit.src,
						   0, 0,
						   IPPROTO_IPV6, 0,
						   0);
			if (IS_ERR(rt) ||
			    rt->rt_type != RTN_UNICAST)
				return false;

			ffn6_priv = ffn6_get_rw_priv(e6);
			if (!ffn6_priv)
				return false;

			/* cache this inside FFN private area */
			ffn6_priv->tun_dst = (struct dst_entry *)rt;
			ffn6_priv->tun_dest_ip = dest;
			e6->manip.priv_destructor = ffn6_priv_destructor_cb;

			v4_dst = (struct dst_entry *)rt;

cached_ipv4_route:
			if (v4_dst->dev != tx_dev) {
				return false;
			}

			/* is the neighboor ready ? */
			rt = (struct rtable *)v4_dst;
			nexthop = (__force u32)rt_nexthop(rt, dest);
			neigh = __ipv4_neigh_lookup_noref(tx_dev, nexthop);
			if (!neigh) {
				return false;
			}

			tun_v4_dest = dest;
			xmit_mode = FF_XMIT_IPV6_IN_IPV4;

		} else if (e6->manip.dst->dev == tx_dev) {
			struct in6_addr *nexthop;
			struct rt6_info *rt6;

			/* is the neighboor ready ? */
			rt6 = (struct rt6_info *)e6->manip.dst;

			nexthop = rt6_nexthop(rt6,
				      (struct in6_addr *)e6->manip.new_dip);
			if (!nexthop)
				return false;

			neigh = __ipv6_neigh_lookup_noref(tx_dev, nexthop);
			if (!neigh) {
				return false;
			}

			xmit_mode = FF_XMIT_IPV6;
		} else
			return false;
	}

	if (!(neigh->nud_state & NUD_VALID)) {
		return false;
	}

	/* is destination on correct tx bridge port ? */
	if (is_bridge_dev(tx_dev)) {
		struct net_bridge_port *p = br_port_get_rcu(tx_mp->dev);
		struct net_bridge_fdb_entry *fdb;

		fdb = br_fdb_find(p->br, neigh->ha, 0);
		if (!fdb)
			return false;

		if (fdb->dst != p)
			return false;
	}

	txq = tx_mp->ff_txq;
	if (is_bridge_dev(rx_dev)) {
		struct net_bridge *br = netdev_priv(rx_dev);
		struct net_bridge_port *p;
		struct pcpu_sw_netstats *stats;

		/* if packet comes from a bridge, make sure we are
		 * allowed to ingress it */
		p = br_port_get_rcu(rx_mp->dev);
		if (p->state != BR_STATE_FORWARDING) {
			return false;
		}

		/* refresh FDB entry for this source */
		if (!br_fdb_update_only(br, p, eth->h_source)) {
			return false;
		}

		stats = this_cpu_ptr(br->stats);
		stats->rx_packets++;
		stats->rx_bytes += eth_len;

	} else if (rx_vlan) {
		struct vlan_dev_priv *vlan = vlan_dev_priv(rx_dev);
		struct vlan_pcpu_stats *stats;
		stats = this_cpu_ptr(vlan->vlan_pcpu_stats);
		stats->rx_packets++;
		stats->rx_bytes += eth_len;
	} else {
		rx_dev->stats.rx_packets++;
		rx_dev->stats.rx_bytes += eth_len;
	}

	rx_hw_stats = &rx_mp->dev->stats;
	rx_hw_stats->rx_bytes += eth_len;
	rx_hw_stats->rx_packets++;

	/* do we have room in the tx queue ? */
	if (txq->tx_desc_count == txq->tx_ring_size &&
	    !ff_tx_queue_can_reclaim(txq)) {
		/* just rearm descriptor and fake success */
		rx_desc->cmd_sts = BUFFER_OWNED_BY_DMA | TX_ENABLE_INTERRUPT;
		txq_enable(txq);
		return true;
	}

	/* remember RX desc hw address before we reload it and point
	 * if back to frag hw address */
	buf_addr = rx_desc->buf_ptr;
	buf_addr -= offset;

	/* can we allocate a new fragment to replace the descriptor we
	 * are about to use ? */
	if (rx_desc_refill(rx_mp, rx_desc, true)) {
		/* just rearm descriptor and fake success */
		rx_desc->cmd_sts = BUFFER_OWNED_BY_DMA | RX_ENABLE_INTERRUPT;
		return true;
	}

	if (l4_is_tcp) {
		/* don't try to track window anymore on this
		 * connection */
		ct->proto.tcp.no_window_track = 1;
	}

	/* alter l3 & l4 content if needed */
	if (l3_is_ipv4) {
		struct iphdr *iph = (struct iphdr *)l3_hdr;

		if (e->manip.alter) {
			if (l4_is_tcp) {
				struct tcphdr *tcph = (struct tcphdr *)l4_hdr;
				tcph->source = e->manip.new_sport;
				tcph->dest = e->manip.new_dport;
				tcph->check = csum16_sub(tcph->check,
						 e->manip.l4_adjustment);
			} else {
				struct udphdr *udph = (struct udphdr *)l4_hdr;
				udph->source = e->manip.new_sport;
				udph->dest = e->manip.new_dport;
				if (udph->check) {
					u16 tcheck;

					tcheck = csum16_sub(udph->check,
						    e->manip.l4_adjustment);
					udph->check = tcheck ? tcheck : 0xffff;
				}
			}

			iph->saddr = e->manip.new_sip;
			iph->daddr = e->manip.new_dip;
		}

		iph->ttl--;
		iph->check = csum16_sub(iph->check,
					e->manip.ip_adjustment);

	} else {
		struct ipv6hdr *ip6hdr = (struct ipv6hdr *)l3_hdr;

		if (e6->manip.alter) {
			if (l4_is_tcp) {
				struct tcphdr *tcph = (struct tcphdr *)l4_hdr;
				tcph->source = e6->manip.new_sport;
				tcph->dest = e6->manip.new_dport;
				tcph->check = csum16_sub(tcph->check,
							 e6->manip.adjustment);
			} else {
				struct udphdr *udph = (struct udphdr *)l4_hdr;
				udph->source = e6->manip.new_sport;
				udph->dest = e6->manip.new_dport;

				if (udph->check) {
					u16 tcheck;

					tcheck = csum16_sub(udph->check,
						    e6->manip.adjustment);
					udph->check = tcheck ? tcheck : 0xffff;
				}
			}

			memcpy(ip6hdr->saddr.s6_addr32, e6->manip.new_sip, 16);
			memcpy(ip6hdr->daddr.s6_addr32, e6->manip.new_dip, 16);
		}

		ip6hdr->hop_limit--;
	}

	/* packet is ready to xmit */
	switch (xmit_mode) {
	case FF_XMIT_IPV4:
		clean_len = sizeof (struct iphdr);
		proto = ETH_P_IP;
		break;

	case FF_XMIT_IPV6:
		clean_len = sizeof (struct ipv6hdr);
		proto = ETH_P_IPV6;
		break;

	case FF_XMIT_IPV6_IN_IPV4:
	{
		struct iphdr *tun_hdr;
		/* prepend IPv4 */
		tun_hdr = (struct iphdr *)((u8 *)l3_hdr - sizeof (*tun_hdr));
		tun_hdr->ihl = 5;
		tun_hdr->version = 4;
		tun_hdr->tos = 0;
		tun_hdr->tot_len = htons(l3_plen + sizeof (*tun_hdr));
		tun_hdr->id = 0;
		tun_hdr->frag_off = 0;
		tun_hdr->ttl = 64;
		tun_hdr->protocol = IPPROTO_IPV6;
		tun_hdr->saddr = ff.u.sit.src;
		tun_hdr->daddr = tun_v4_dest;

		l3_hdr = (u8 *)tun_hdr;
		l3_plen += sizeof (*tun_hdr);

		clean_len = sizeof (struct iphdr) + sizeof (struct ipv6hdr);
		proto = ETH_P_IP;
		break;
	}

	case FF_XMIT_IPV4_IN_IPV6:
	{
		struct ipv6hdr *tun_6hdr;

		/* prepend IPv6 */
		tun_6hdr = (struct ipv6hdr *)((u8 *)l3_hdr - sizeof (*tun_6hdr));
		tun_6hdr->version = 6;
		tun_6hdr->priority = 0;
		memset(tun_6hdr->flow_lbl, 0, sizeof (tun_6hdr->flow_lbl));
		tun_6hdr->payload_len = htons(l3_plen);
		tun_6hdr->nexthdr = IPPROTO_IPIP;
		tun_6hdr->hop_limit = 64;
		tun_6hdr->saddr = ff.u.map.src;
		tun_6hdr->daddr = *tun_v6_pdest;

		l3_hdr = (u8 *)tun_6hdr;
		l3_plen += sizeof (*tun_6hdr);

		clean_len = sizeof (struct ipv6hdr) + sizeof (struct iphdr);
		proto = ETH_P_IPV6;
		break;
	}
	}

	if (l4_is_tcp)
		clean_len += sizeof (struct tcphdr);
	else
		clean_len += sizeof (struct udphdr);

	/* add ethernet header */
	if (tx_vlan) {
		struct vlan_ethhdr *vhdr;

		vhdr = (struct vlan_ethhdr *)((u8 *)l3_hdr - VLAN_ETH_HLEN);
		memcpy(vhdr->h_dest, neigh->ha, 6);
		memcpy(vhdr->h_source, rx_mp->dev->dev_addr, 6);
		vhdr->h_vlan_proto = htons(ETH_P_8021Q);
		vhdr->h_vlan_TCI = htons(836);
		vhdr->h_vlan_encapsulated_proto = htons(proto);

		eth = (struct ethhdr *)vhdr;
		eth_len = l3_plen + VLAN_ETH_HLEN;
		clean_len += VLAN_ETH_HLEN;
	} else {
		eth = (struct ethhdr *)((u8 *)l3_hdr - ETH_HLEN);
		memcpy(eth->h_dest, neigh->ha, 6);
		memcpy(eth->h_source, rx_mp->dev->dev_addr, 6);
		eth->h_proto = htons(proto);
		eth_len = l3_plen + ETH_HLEN;
		clean_len += ETH_HLEN;
	}

	if (ff_send(txq,
		    buf_addr + (void *)eth - frag,
		    frag, rx_mp->pkt_size,
		    eth_len,
		    clean_len,
		    (proto == ETH_P_IP),
		    (xmit_mode == FF_XMIT_IPV4),
		    l4_is_tcp,
		    tx_vlan)) {
		skb_free_frag(frag);
		return true;
	}

	if (is_bridge_dev(tx_dev)) {
		struct net_bridge *br = netdev_priv(tx_dev);
		struct pcpu_sw_netstats *stats;
		stats = this_cpu_ptr(br->stats);
		stats->tx_packets++;
		stats->tx_bytes += eth_len;
	} else if (tx_vlan) {
		struct vlan_dev_priv *vlan = vlan_dev_priv(tx_dev);
		struct vlan_pcpu_stats *stats;
		stats = this_cpu_ptr(vlan->vlan_pcpu_stats);
		stats->tx_packets++;
		stats->tx_bytes += eth_len;
	} else {
		tx_dev->stats.tx_packets++;
		tx_dev->stats.tx_bytes += eth_len;
	}

	tx_hw_stats = &tx_mp->dev->stats;
	tx_hw_stats->tx_bytes += eth_len;
	tx_hw_stats->tx_packets++;

	/* refresh conntrack */
	if (l4_is_tcp)
		timeout = HZ * 3600 * 24 * 5;
	else
		timeout = HZ * 180;

	if (ct->timeout.expires - ff.jiffies < timeout - 10 * HZ) {
		unsigned long newtime = ff.jiffies + timeout;
		mod_timer_pending(&ct->timeout, newtime);
	}

	return true;
}

/*
 *
 */
static bool ff_receive(struct mv643xx_eth_private *mp,
		       struct rx_desc *rx_desc,
		       unsigned int cmd_sts,
		       void *frag,
		       size_t offset, size_t dlen)
{
#ifdef CONFIG_FBXBRIDGE
	if (mp->dev->fbx_bridge_maybe_port)
		return false;
#endif

	if (!ff_enabled)
		return false;

	/*
	 * GWv1
	 */
	if (ff_mode == 1) {
		/*
		 * LAN => WAN
		 * [eth0 (untagged)] => [br0] => IPV4 => [eth1.836]
		 *
		 * WAN => LAN
		 * [eth1.836] => IPV4 => [br0] => [eth0]
		 */
		if (mp->shared->unit == 0)
			return ff_forward(mp, mp_by_unit[1],
					  ff.lan_dev,
					  ff.wan_dev,
					  0, 836,
					  rx_desc,
					  cmd_sts,
					  frag, offset, dlen);

		if (mp->shared->unit == 1)
			return ff_forward(mp, mp_by_unit[0],
					  ff.wan_dev,
					  ff.lan_dev,
					  836, 0,
					  rx_desc,
					  cmd_sts,
					  frag, offset, dlen);
	}

	/*
	 * GWv2
	 */
	if (ff_mode == 2) {
		/*
		 * LAN => WAN
		 * [eth0 (untagged)] => [br0] => IPV4 => [eth0.836]
		 *
		 * WAN => LAN
		 * [eth0.836] => IPV4 => [br0] => [eth0]
		 */
		if (mp->shared->unit != 0)
			return false;

		if (!pkt_is_vlan(cmd_sts))
			return ff_forward(mp, mp,
					  ff.lan_dev,
					  ff.wan_dev,
					  0, 836,
					  rx_desc,
					  cmd_sts,
					  frag, offset, dlen);
		else
			return ff_forward(mp, mp,
					  ff.wan_dev,
					  ff.lan_dev,
					  836, 0,
					  rx_desc,
					  cmd_sts,
					  frag, offset, dlen);
	}

	return false;
}

/*
 *
 */
static ssize_t ff_show_enabled(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%u\n", ff_enabled);
}

static ssize_t ff_store_enabled(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (ff_enabled == val)
		return len;

	printk(KERN_NOTICE "ff: fastpath now %s\n",
	       val ? "enabled" : "disabled");
	ff_enabled = val;
	return len;
}

static struct device_attribute dev_attr_ff = {
	.attr = { .name = "ff_enabled", .mode = (S_IRUGO | S_IWUSR) },
	.show = ff_show_enabled,
	.store = ff_store_enabled,
};

/*
 *
 */
static ssize_t ff_show_tun_dev(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%u\n", ff_enabled);
}

static ssize_t ff_store_tun_dev(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	if (!len || buf[0] == '\n') {
		ff.tun_dev_name[0] = 0;
		ff_tun_release();
		printk(KERN_NOTICE "ff: tun dev unset\n");
		return len;
	}

	strncpy(ff.tun_dev_name, buf, len);
	strim(ff.tun_dev_name);
	printk(KERN_NOTICE "ff: tun dev set to %s\n", ff.tun_dev_name);
	ff_tun_capture();
	return len;
}

static struct device_attribute dev_attr_tun = {
	.attr = { .name = "ff_tun_dev", .mode = (S_IRUGO | S_IWUSR) },
	.show = ff_show_tun_dev,
	.store = ff_store_tun_dev,
};

static struct ff_dev gw_lan = {
	.desc			= "lan",
	.unit			= 0,
	.bridge_member		= true,
	.pvirt_dev		= &ff.lan_dev,
};

static struct ff_dev gwv1_wan = {
	.desc			= "wan",
	.unit			= 1,
	.vlan			= 836,
	.pvirt_dev		= &ff.wan_dev,
};

static struct ff_dev gwv2_wan = {
	.desc			= "wan",
	.unit			= 0,
	.vlan			= 836,
	.pvirt_dev		= &ff.wan_dev,
};

static void ff_init(struct device *dev)
{
	static bool done;

	if (done)
		return;

	device_create_file(dev, &dev_attr_ff);
	device_create_file(dev, &dev_attr_tun);

	printk(KERN_DEBUG "ff_init: mode %u\n", ff_mode);
	switch (ff_mode) {
	case 1:
		list_add(&gw_lan.next, &ff_devs);
		list_add(&gwv1_wan.next, &ff_devs);
		break;

	case 2:
		list_add(&gw_lan.next, &ff_devs);
		list_add(&gwv2_wan.next, &ff_devs);
		break;
	}

	done = true;
}
#endif

static void rxq_receive_packet(struct mv643xx_eth_private *mp,
			       struct rx_queue *rxq,
			       unsigned int cmd_sts,
			       struct sk_buff *skb)
{
	struct net_device_stats *stats = &mp->dev->stats;

	if (cmd_sts & LAYER_4_CHECKSUM_OK)
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	rxq->rx_packets++;
	stats->rx_packets++;
	stats->rx_bytes += skb->len + ETH_FCS_LEN;

#ifdef CONFIG_FBXBRIDGE
	if (mp->dev->fbx_bridge_maybe_port &&
	    pkt_is_vlan(cmd_sts) &&
	    pkt_is_ipv4(cmd_sts)) {

		if (pkt_is_tcp4(cmd_sts)) {
			if (__fbxbridge_fp_in_vlan_tcp4(mp->dev, skb))
				return;
		} else if (pkt_is_udp4(cmd_sts)) {
			if (__fbxbridge_fp_in_vlan_udp4(mp->dev, skb))
				return;
		}
	}
#endif

	skb->protocol = eth_type_trans(skb, mp->dev);

	if (pkt_is_ipv4(cmd_sts) &&
	    (pkt_is_udp4(cmd_sts) || pkt_is_tcp4(cmd_sts)))
		napi_gro_receive(&mp->napi, skb);
	else
		netif_receive_skb(skb);
}

static int rxq_process(struct rx_queue *rxq, int budget)
{
	struct mv643xx_eth_private *mp = rxq_to_mp(rxq);
	struct net_device_stats *stats = &mp->dev->stats;
	int rx_done;

	rx_done = 0;
	while (rx_done < budget) {
		struct rx_desc *rx_desc;
		struct sk_buff *skb;
		unsigned int cmd_sts;
		void *frag;
		u16 byte_cnt;
		int ret;

		rx_desc = &rxq->rx_desc_area[rxq->rx_curr_desc];

		cmd_sts = rx_desc->cmd_sts;
		if (cmd_sts & BUFFER_OWNED_BY_DMA)
			break;

		rx_done++;
		rxq->rx_curr_desc++;
		if (rxq->rx_curr_desc == rxq->rx_ring_size)
			rxq->rx_curr_desc = 0;

		/*
		 * In case we received a packet without first / last bits
		 * on, or the error summary bit is set, the packet needs
		 * to be dropped.
		 */
		if ((cmd_sts & (RX_FIRST_DESC | RX_LAST_DESC | ERROR_SUMMARY))
		    != (RX_FIRST_DESC | RX_LAST_DESC))
			goto err_rearm;

		rmb();
		frag = (void *)rx_desc->cookie;
		byte_cnt = rx_desc->byte_cnt;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
		if (ff_receive(mp, rx_desc, cmd_sts,
			       frag, RX_OFFSET,
			       byte_cnt - ETH_FCS_LEN)) {
			rxq->rx_packets++;
			continue;
		}
#endif

		if (byte_cnt <= COPY_BREAK_SIZE) {
			/* better copy a small frame and not unmap the
			 * DMA region */
			skb = netdev_alloc_skb_ip_align(mp->dev, byte_cnt);
			if (unlikely(!skb))
				goto err_rearm;

			dma_sync_single_range_for_cpu(mp->dev->dev.parent,
						      rx_desc->buf_ptr,
						      0,
						      byte_cnt,
						      DMA_FROM_DEVICE);

			memcpy(skb_put(skb, byte_cnt - 2 - ETH_FCS_LEN),
			       frag + RX_OFFSET + 2,
			       byte_cnt - 2 - ETH_FCS_LEN);

			dma_sync_single_range_for_device(mp->dev->dev.parent,
							 rx_desc->buf_ptr,
							 0,
							 byte_cnt,
							 DMA_FROM_DEVICE);

			/* rearm descriptor */
			rx_desc->cmd_sts = BUFFER_OWNED_BY_DMA |
				RX_ENABLE_INTERRUPT;

			rxq_receive_packet(mp, rxq, cmd_sts, skb);
			continue;
		}

                ret = rx_desc_refill(mp, rx_desc, true);
		if (ret) {
			netdev_err(mp->dev, "oom while refill\n");
			goto err_rearm;
		}

		/* descriptor is re-armed now */

		skb = build_skb(frag, mp->frag_size > PAGE_SIZE ?
				0 : mp->frag_size);
		if (!skb) {
			mv643xx_eth_frag_free(mp, frag);
			stats->rx_dropped++;
			continue;
		}

		/* add NET_SKB_PAD + skip 2 bytes of hardware align */
		skb_reserve(skb, RX_OFFSET + 2);
		skb_put(skb, byte_cnt - 2 - ETH_FCS_LEN);

		rxq_receive_packet(mp, rxq, cmd_sts, skb);
		continue;

err_rearm:
		stats->rx_dropped++;

		if ((cmd_sts & (RX_FIRST_DESC | RX_LAST_DESC)) !=
			(RX_FIRST_DESC | RX_LAST_DESC)) {
			if (net_ratelimit())
				netdev_err(mp->dev,
					   "received packet spanning multiple descriptors\n");
		}

		if (cmd_sts & ERROR_SUMMARY) {
			stats->rx_errors++;
			if (cmd_sts & RX_FIRST_DESC) {
				switch (cmd_sts & ERROR_CODE_MASK) {
				case ERROR_CODE_RX_MAX_LENGTH:
					stats->rx_length_errors++;
					break;
				case ERROR_CODE_RX_CRC:
					stats->rx_crc_errors++;
					break;
				case ERROR_CODE_RX_OVERRUN:
					stats->rx_fifo_errors++;
					break;
	}
}
		}

		/* rearm descriptor */
		rx_desc->cmd_sts = BUFFER_OWNED_BY_DMA | RX_ENABLE_INTERRUPT;
	}

	if (rx_done < budget)
		mp->work_rx &= ~(1 << rxq->index);

	return rx_done;
}


/* tx ***********************************************************************/
static inline unsigned int has_tiny_unaligned_frags(struct sk_buff *skb)
{
	int frag;

	for (frag = 0; frag < skb_shinfo(skb)->nr_frags; frag++) {
		const skb_frag_t *fragp = &skb_shinfo(skb)->frags[frag];

		if (skb_frag_size(fragp) <= 8 && fragp->page_offset & 7)
			return 1;
	}

	return 0;
}

static inline __be16 sum16_as_be(__sum16 sum)
{
	return (__force __be16)sum;
}

static int skb_tx_csum(struct mv643xx_eth_private *mp, struct sk_buff *skb,
		       u16 *l4i_chk, u32 *command, int length)
{
	int ret;
	u32 cmd = 0;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		int hdr_len;
		int tag_bytes;

		BUG_ON(skb->protocol != htons(ETH_P_IP) &&
		       skb->protocol != htons(ETH_P_8021Q));

		hdr_len = (void *)ip_hdr(skb) - (void *)skb->data;
		tag_bytes = hdr_len - ETH_HLEN;

		if (length - hdr_len > mp->shared->tx_csum_limit ||
		    unlikely(tag_bytes & ~12)) {
			ret = skb_checksum_help(skb);
			if (!ret)
				goto no_csum;
			return ret;
		}

		if (tag_bytes & 4)
			cmd |= MAC_HDR_EXTRA_4_BYTES;
		if (tag_bytes & 8)
			cmd |= MAC_HDR_EXTRA_8_BYTES;

		cmd |= GEN_TCP_UDP_CHECKSUM | GEN_TCP_UDP_CHK_FULL |
			   GEN_IP_V4_CHECKSUM   |
			   ip_hdr(skb)->ihl << TX_IHL_SHIFT;

		/* TODO: Revisit this. With the usage of GEN_TCP_UDP_CHK_FULL
		 * it seems we don't need to pass the initial checksum. */
		switch (ip_hdr(skb)->protocol) {
		case IPPROTO_UDP:
			cmd |= UDP_FRAME;
			*l4i_chk = 0;
			break;
		case IPPROTO_TCP:
			*l4i_chk = 0;
			break;
		default:
			WARN(1, "protocol not supported");
		}
	} else {
no_csum:
		/* Errata BTS #50, IHL must be 5 if no HW checksum */
		cmd |= 5 << TX_IHL_SHIFT;
	}
	*command = cmd;
	return 0;
}

static inline int
txq_put_data_tso(struct net_device *dev, struct tx_queue *txq,
		 struct sk_buff *skb, char *data, int length,
		 bool last_tcp, bool is_last)
{
	int tx_index;
	u32 cmd_sts;
	struct tx_desc *desc;

	tx_index = txq->tx_curr_desc++;
	if (txq->tx_curr_desc == txq->tx_ring_size)
		txq->tx_curr_desc = 0;
	desc = &txq->tx_desc_area[tx_index];
	txq->tx_desc_mapping[tx_index] = DESC_DMA_MAP_SINGLE;

	desc->l4i_chk = 0;
	desc->byte_cnt = length;

	if (length <= 8 && (uintptr_t)data & 0x7) {
		/* Copy unaligned small data fragment to TSO header data area */
		memcpy(txq->tso_hdrs + txq->tx_curr_desc * TSO_HEADER_SIZE,
		       data, length);
		desc->buf_ptr = txq->tso_hdrs_dma
			+ txq->tx_curr_desc * TSO_HEADER_SIZE;
	} else {
		/* Alignment is okay, map buffer and hand off to hardware */
		txq->tx_desc_mapping[tx_index] = DESC_DMA_MAP_SINGLE;
		desc->buf_ptr = dma_map_single(dev->dev.parent, data,
			length, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dev->dev.parent,
					       desc->buf_ptr))) {
			WARN(1, "dma_map_single failed!\n");
			return -ENOMEM;
		}
	}

	cmd_sts = BUFFER_OWNED_BY_DMA;
	if (last_tcp) {
		/* last descriptor in the TCP packet */
		cmd_sts |= ZERO_PADDING | TX_LAST_DESC;
		/* last descriptor in SKB */
		if (is_last)
			cmd_sts |= TX_ENABLE_INTERRUPT;
	}
	desc->cmd_sts = cmd_sts;
	return 0;
}

static inline void
txq_put_hdr_tso(struct sk_buff *skb, struct tx_queue *txq, int length,
		u32 *first_cmd_sts, bool first_desc)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	int tx_index;
	struct tx_desc *desc;
	int ret;
	u32 cmd_csum = 0;
	u16 l4i_chk = 0;
	u32 cmd_sts;

	tx_index = txq->tx_curr_desc;
	desc = &txq->tx_desc_area[tx_index];

	ret = skb_tx_csum(mp, skb, &l4i_chk, &cmd_csum, length);
	if (ret)
		WARN(1, "failed to prepare checksum!");

	/* Should we set this? Can't use the value from skb_tx_csum()
	 * as it's not the correct initial L4 checksum to use. */
	desc->l4i_chk = 0;

	desc->byte_cnt = hdr_len;
	desc->buf_ptr = txq->tso_hdrs_dma +
			txq->tx_curr_desc * TSO_HEADER_SIZE;
	cmd_sts = cmd_csum | BUFFER_OWNED_BY_DMA  | TX_FIRST_DESC |
				   GEN_CRC;

	/* Defer updating the first command descriptor until all
	 * following descriptors have been written.
	 */
	if (first_desc)
		*first_cmd_sts = cmd_sts;
	else
		desc->cmd_sts = cmd_sts;

	txq->tx_curr_desc++;
	if (txq->tx_curr_desc == txq->tx_ring_size)
		txq->tx_curr_desc = 0;
}

static int txq_submit_tso(struct tx_queue *txq, struct sk_buff *skb,
			  struct net_device *dev)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int total_len, data_left, ret;
	int desc_count = 0;
	struct tso_t tso;
	int hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	struct tx_desc *first_tx_desc;
	u32 first_cmd_sts = 0;

	/* Count needed descriptors */
	if ((txq->tx_desc_count + tso_count_descs(skb)) >= txq->tx_ring_size) {
		netdev_dbg(dev, "not enough descriptors for TSO!\n");
		return -EBUSY;
	}

	first_tx_desc = &txq->tx_desc_area[txq->tx_curr_desc];

	/* Initialize the TSO handler, and prepare the first payload */
	tso_start(skb, &tso);

	total_len = skb->len - hdr_len;
	while (total_len > 0) {
		bool first_desc = (desc_count == 0);
		char *hdr;

		data_left = min_t(int, skb_shinfo(skb)->gso_size, total_len);
		total_len -= data_left;
		desc_count++;

		/* prepare packet headers: MAC + IP + TCP */
		hdr = txq->tso_hdrs + txq->tx_curr_desc * TSO_HEADER_SIZE;
		tso_build_hdr(skb, hdr, &tso, data_left, total_len == 0);
		txq_put_hdr_tso(skb, txq, data_left, &first_cmd_sts,
				first_desc);

		while (data_left > 0) {
			int size;
			desc_count++;

			size = min_t(int, tso.size, data_left);
			ret = txq_put_data_tso(dev, txq, skb, tso.data, size,
					       size == data_left,
					       total_len == 0);
			if (ret)
				goto err_release;
			data_left -= size;
			tso_build_data(skb, &tso, size);
		}
	}

	__skb_queue_tail(&txq->tx_skb, skb);
	skb_tx_timestamp(skb);

	/* ensure all other descriptors are written before first cmd_sts */
	wmb();
	first_tx_desc->cmd_sts = first_cmd_sts;

	/* clear TX_END status */
	mp->work_tx_end &= ~(1 << txq->index);

	txq_enable(txq);
	txq->tx_desc_count += desc_count;
	return 0;
err_release:
	/* TODO: Release all used data descriptors; header descriptors must not
	 * be DMA-unmapped.
	 */
	return ret;
}

static void txq_submit_frag_skb(struct tx_queue *txq, struct sk_buff *skb)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int nr_frags = skb_shinfo(skb)->nr_frags;
	int frag;

	for (frag = 0; frag < nr_frags; frag++) {
		skb_frag_t *this_frag;
		int tx_index;
		struct tx_desc *desc;

		this_frag = &skb_shinfo(skb)->frags[frag];
		tx_index = txq->tx_curr_desc++;
		if (txq->tx_curr_desc == txq->tx_ring_size)
			txq->tx_curr_desc = 0;
		desc = &txq->tx_desc_area[tx_index];
		txq->tx_desc_mapping[tx_index] = DESC_DMA_MAP_PAGE;

		/*
		 * The last fragment will generate an interrupt
		 * which will free the skb on TX completion.
		 */
		if (frag == nr_frags - 1) {
			desc->cmd_sts = BUFFER_OWNED_BY_DMA |
					ZERO_PADDING | TX_LAST_DESC |
					TX_ENABLE_INTERRUPT;
		} else {
			desc->cmd_sts = BUFFER_OWNED_BY_DMA;
		}

		desc->l4i_chk = 0;
		desc->byte_cnt = skb_frag_size(this_frag);
		desc->buf_ptr = skb_frag_dma_map(mp->dev->dev.parent,
						 this_frag, 0, desc->byte_cnt,
						 DMA_TO_DEVICE);
	}
}

static int txq_submit_skb(struct tx_queue *txq, struct sk_buff *skb,
			  struct net_device *dev)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int nr_frags = skb_shinfo(skb)->nr_frags;
	int tx_index;
	struct tx_desc *desc;
	u32 cmd_sts;
	u16 l4i_chk;
	int maplen, length, ret;

	cmd_sts = 0;
	l4i_chk = 0;

	if (txq->tx_ring_size - txq->tx_desc_count < MAX_SKB_FRAGS + 1) {
		if (net_ratelimit())
			netdev_err(dev, "tx queue full?!\n");
		return -EBUSY;
	}

	ret = skb_tx_csum(mp, skb, &l4i_chk, &cmd_sts, skb->len);
	if (ret)
		return ret;
	cmd_sts |= TX_FIRST_DESC | GEN_CRC | BUFFER_OWNED_BY_DMA;

	tx_index = txq->tx_curr_desc++;
	if (txq->tx_curr_desc == txq->tx_ring_size)
		txq->tx_curr_desc = 0;
	desc = &txq->tx_desc_area[tx_index];
	txq->tx_desc_mapping[tx_index] = DESC_DMA_MAP_SINGLE;

	if (nr_frags) {
		txq_submit_frag_skb(txq, skb);
		length = skb_headlen(skb);
	} else {
		cmd_sts |= ZERO_PADDING | TX_LAST_DESC | TX_ENABLE_INTERRUPT;
		length = skb->len;
	}

	maplen = length;
#ifdef CONFIG_FBXBRIDGE
	if (skb->fbxbridge_state == 2 && maplen > COPY_BREAK_SIZE)
		maplen = COPY_BREAK_SIZE;
#endif

	desc->l4i_chk = l4i_chk;
	desc->byte_cnt = length;
	desc->buf_ptr = dma_map_single(mp->dev->dev.parent, skb->data,
				       maplen, DMA_TO_DEVICE);

	__skb_queue_tail(&txq->tx_skb, skb);

	skb_tx_timestamp(skb);

	/* ensure all other descriptors are written before first cmd_sts */
	wmb();
	desc->cmd_sts = cmd_sts;

	/* clear TX_END status */
	mp->work_tx_end &= ~(1 << txq->index);

	/* ensure all descriptors are written before poking hardware */
	wmb();
	txq_enable(txq);

	txq->tx_desc_count += nr_frags + 1;

	return 0;
}

static netdev_tx_t mv643xx_eth_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int length, queue, ret;
	struct tx_queue *txq;
	struct netdev_queue *nq;

	queue = skb_get_queue_mapping(skb);
	txq = mp->txq + queue + NAPI_TX_OFFSET;
	nq = netdev_get_tx_queue(dev, queue);

	if (has_tiny_unaligned_frags(skb) && __skb_linearize(skb)) {
		netdev_printk(KERN_DEBUG, dev,
			      "failed to linearize skb with tiny unaligned fragment\n");
		return NETDEV_TX_BUSY;
	}

	length = skb->len;

	if (skb_is_gso(skb))
		ret = txq_submit_tso(txq, skb, dev);
	else
		ret = txq_submit_skb(txq, skb, dev);
	if (!ret) {
		txq->tx_bytes += length;
		txq->tx_packets++;

		if (txq->tx_desc_count >= txq->tx_stop_threshold)
			netif_tx_stop_queue(nq);
	} else {
		txq->tx_dropped++;
		dev_kfree_skb_any(skb);
	}

	return NETDEV_TX_OK;
}


/* tx napi ******************************************************************/
static void txq_kick(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	struct netdev_queue *nq;
	u32 hw_desc_ptr;
	u32 expected_ptr;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	WARN_ON(txq->index == 0);
#endif
	nq = netdev_get_tx_queue(mp->dev, txq->index - NAPI_TX_OFFSET);
	__netif_tx_lock(nq, smp_processor_id());

	if (rdlp(mp, TXQ_COMMAND) & (1 << txq->index))
		goto out;

	hw_desc_ptr = rdlp(mp, TXQ_CURRENT_DESC_PTR(txq->index));
	expected_ptr = (u32)txq->tx_desc_dma +
				txq->tx_curr_desc * sizeof(struct tx_desc);

	if (hw_desc_ptr != expected_ptr)
		txq_enable(txq);

out:
	__netif_tx_unlock(nq);

	mp->work_tx_end &= ~(1 << txq->index);
}

static int txq_reclaim(struct tx_queue *txq, int budget, int force)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	struct netdev_queue *nq;
	int reclaimed;

	nq = NULL;
#ifdef CONFIG_MV643XX_ETH_FBX_FF
	if (txq->index != 0)
#endif
		nq = netdev_get_tx_queue(mp->dev, txq->index - NAPI_TX_OFFSET);

	if (nq)
	__netif_tx_lock_bh(nq);

	reclaimed = 0;
	while (reclaimed < budget && txq->tx_desc_count > 0) {
		int tx_index;
		struct tx_desc *desc;
		u32 cmd_sts;
		char desc_dma_map;

		tx_index = txq->tx_used_desc;
		desc = &txq->tx_desc_area[tx_index];
		desc_dma_map = txq->tx_desc_mapping[tx_index];

		cmd_sts = desc->cmd_sts;

		if (cmd_sts & BUFFER_OWNED_BY_DMA) {
			if (!force)
				break;
			desc->cmd_sts = cmd_sts & ~BUFFER_OWNED_BY_DMA;
		}

		txq->tx_used_desc = tx_index + 1;
		if (txq->tx_used_desc == txq->tx_ring_size)
			txq->tx_used_desc = 0;

		reclaimed++;
		txq->tx_desc_count--;

		if (!IS_TSO_HEADER(txq, desc->buf_ptr)) {

			if (desc_dma_map == DESC_DMA_MAP_PAGE)
				dma_unmap_page(mp->dev->dev.parent,
					       desc->buf_ptr,
					       desc->byte_cnt,
					       DMA_TO_DEVICE);
			else
				dma_unmap_single(mp->dev->dev.parent,
						 desc->buf_ptr,
						 desc->byte_cnt,
						 DMA_TO_DEVICE);
		}

		if (cmd_sts & TX_ENABLE_INTERRUPT) {
			struct sk_buff *skb = __skb_dequeue(&txq->tx_skb);

			if (!WARN_ON(!skb))
				dev_kfree_skb(skb);
		}

		if (cmd_sts & ERROR_SUMMARY) {
			netdev_info(mp->dev, "tx error\n");
			mp->dev->stats.tx_errors++;
		}

	}

	if (nq)
	__netif_tx_unlock_bh(nq);

	if (reclaimed < budget)
		mp->work_tx &= ~(1 << txq->index);

	return reclaimed;
}


/* tx rate control **********************************************************/
/*
 * Set total maximum TX rate (shared by all TX queues for this port)
 * to 'rate' bits per second, with a maximum burst of 'burst' bytes.
 */
static void tx_set_rate(struct mv643xx_eth_private *mp, int rate, int burst)
{
	int token_rate;
	int mtu;
	int bucket_size;

	token_rate = ((rate / 1000) * 64) / (mp->t_clk / 1000);
	if (token_rate > 1023)
		token_rate = 1023;

	mtu = (mp->dev->mtu + 255) >> 8;
	if (mtu > 63)
		mtu = 63;

	bucket_size = (burst + 255) >> 8;
	if (bucket_size > 65535)
		bucket_size = 65535;

	switch (mp->shared->tx_bw_control) {
	case TX_BW_CONTROL_OLD_LAYOUT:
		wrlp(mp, TX_BW_RATE, token_rate);
		wrlp(mp, TX_BW_MTU, mtu);
		wrlp(mp, TX_BW_BURST, bucket_size);
		break;
	case TX_BW_CONTROL_NEW_LAYOUT:
		wrlp(mp, TX_BW_RATE_MOVED, token_rate);
		wrlp(mp, TX_BW_MTU_MOVED, mtu);
		wrlp(mp, TX_BW_BURST_MOVED, bucket_size);
		break;
	}
}

static void txq_set_rate(struct tx_queue *txq, int rate, int burst)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int token_rate;
	int bucket_size;

	token_rate = ((rate / 1000) * 64) / (mp->t_clk / 1000);
	if (token_rate > 1023)
		token_rate = 1023;

	bucket_size = (burst + 255) >> 8;
	if (bucket_size > 65535)
		bucket_size = 65535;

	wrlp(mp, TXQ_BW_TOKENS(txq->index), token_rate << 14);
	wrlp(mp, TXQ_BW_CONF(txq->index), (bucket_size << 10) | token_rate);
}

static void txq_set_fixed_prio_mode(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);
	int off;
	u32 val;

	/*
	 * Turn on fixed priority mode.
	 */
	off = 0;
	switch (mp->shared->tx_bw_control) {
	case TX_BW_CONTROL_OLD_LAYOUT:
		off = TXQ_FIX_PRIO_CONF;
		break;
	case TX_BW_CONTROL_NEW_LAYOUT:
		off = TXQ_FIX_PRIO_CONF_MOVED;
		break;
	}

	if (off) {
		val = rdlp(mp, off);
		val |= 1 << txq->index;
		wrlp(mp, off, val);
	}
}


/* mii management interface *************************************************/
static void mv643xx_eth_adjust_link(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	u32 pscr = rdlp(mp, PORT_SERIAL_CONTROL);
	u32 autoneg_disable = FORCE_LINK_PASS |
	             DISABLE_AUTO_NEG_SPEED_GMII |
		     DISABLE_AUTO_NEG_FOR_FLOW_CTRL |
		     DISABLE_AUTO_NEG_FOR_DUPLEX;

	if (mp->phy->autoneg == AUTONEG_ENABLE) {
		/* enable auto negotiation */
		pscr &= ~autoneg_disable;
		goto out_write;
	}

	pscr |= autoneg_disable;

	if (mp->phy->speed == SPEED_1000) {
		/* force gigabit, half duplex not supported */
		pscr |= SET_GMII_SPEED_TO_1000;
		pscr |= SET_FULL_DUPLEX_MODE;
		goto out_write;
	}

	pscr &= ~SET_GMII_SPEED_TO_1000;

	if (mp->phy->speed == SPEED_100)
		pscr |= SET_MII_SPEED_TO_100;
	else
		pscr &= ~SET_MII_SPEED_TO_100;

	if (mp->phy->duplex == DUPLEX_FULL)
		pscr |= SET_FULL_DUPLEX_MODE;
	else
		pscr &= ~SET_FULL_DUPLEX_MODE;

out_write:
	wrlp(mp, PORT_SERIAL_CONTROL, pscr);
}

/* statistics ***************************************************************/
static struct net_device_stats *mv643xx_eth_get_stats(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	unsigned long tx_packets = 0;
	unsigned long tx_bytes = 0;
	unsigned long tx_dropped = 0;
	int i;

	for (i = 0; i < mp->txq_count; i++) {
		struct tx_queue *txq = mp->txq + i;

		tx_packets += txq->tx_packets;
		tx_bytes += txq->tx_bytes;
		tx_dropped += txq->tx_dropped;
	}

	stats->tx_packets = tx_packets;
	stats->tx_bytes = tx_bytes;
	stats->tx_dropped = tx_dropped;

	return stats;
}

static inline u32 mib_read(struct mv643xx_eth_private *mp, int offset)
{
	return rdl(mp, MIB_COUNTERS(mp->port_num) + offset);
}

static void mib_counters_clear(struct mv643xx_eth_private *mp)
{
	int i;

	for (i = 0; i < 0x80; i += 4)
		mib_read(mp, i);

	/* Clear non MIB hw counters also */
	rdlp(mp, RX_DISCARD_FRAME_CNT);
	rdlp(mp, RX_OVERRUN_FRAME_CNT);
}

static void mib_counters_update(struct mv643xx_eth_private *mp)
{
	struct mib_counters *p = &mp->mib_counters;
	unsigned int i;

	spin_lock_bh(&mp->mib_counters_lock);
	p->good_octets_received += mib_read(mp, 0x00);
	p->bad_octets_received += mib_read(mp, 0x08);
	p->internal_mac_transmit_err += mib_read(mp, 0x0c);
	p->good_frames_received += mib_read(mp, 0x10);
	p->bad_frames_received += mib_read(mp, 0x14);
	p->broadcast_frames_received += mib_read(mp, 0x18);
	p->multicast_frames_received += mib_read(mp, 0x1c);
	p->frames_64_octets += mib_read(mp, 0x20);
	p->frames_65_to_127_octets += mib_read(mp, 0x24);
	p->frames_128_to_255_octets += mib_read(mp, 0x28);
	p->frames_256_to_511_octets += mib_read(mp, 0x2c);
	p->frames_512_to_1023_octets += mib_read(mp, 0x30);
	p->frames_1024_to_max_octets += mib_read(mp, 0x34);
	p->good_octets_sent += mib_read(mp, 0x38);
	p->good_frames_sent += mib_read(mp, 0x40);
	p->excessive_collision += mib_read(mp, 0x44);
	p->multicast_frames_sent += mib_read(mp, 0x48);
	p->broadcast_frames_sent += mib_read(mp, 0x4c);
	p->unrec_mac_control_received += mib_read(mp, 0x50);
	p->fc_sent += mib_read(mp, 0x54);
	p->good_fc_received += mib_read(mp, 0x58);
	p->bad_fc_received += mib_read(mp, 0x5c);
	p->undersize_received += mib_read(mp, 0x60);
	p->fragments_received += mib_read(mp, 0x64);
	p->oversize_received += mib_read(mp, 0x68);
	p->jabber_received += mib_read(mp, 0x6c);
	p->mac_receive_error += mib_read(mp, 0x70);
	p->bad_crc_event += mib_read(mp, 0x74);
	p->collision += mib_read(mp, 0x78);
	p->late_collision += mib_read(mp, 0x7c);
	/* Non MIB hardware counters */
	p->rx_discard += rdlp(mp, RX_DISCARD_FRAME_CNT);
	p->rx_overrun += rdlp(mp, RX_OVERRUN_FRAME_CNT);
	/* Non MIB software counters */
	for (i = 0; i < ARRAY_SIZE(mp->rxq); i++)
		p->rx_packets_q[i] = mp->rxq[i].rx_packets;
	for (i = 0; i < ARRAY_SIZE(mp->txq); i++)
		p->tx_packets_q[i] = mp->txq[i].tx_packets;

	spin_unlock_bh(&mp->mib_counters_lock);
}

static void mib_counters_timer_wrapper(unsigned long _mp)
{
	struct mv643xx_eth_private *mp = (void *)_mp;
	mib_counters_update(mp);
	mod_timer(&mp->mib_counters_timer, jiffies + 30 * HZ);
}


/* interrupt coalescing *****************************************************/
/*
 * Hardware coalescing parameters are set in units of 64 t_clk
 * cycles.  I.e.:
 *
 *	coal_delay_in_usec = 64000000 * register_value / t_clk_rate
 *
 *	register_value = coal_delay_in_usec * t_clk_rate / 64000000
 *
 * In the ->set*() methods, we round the computed register value
 * to the nearest integer.
 */
static unsigned int get_rx_coal(struct mv643xx_eth_private *mp)
{
	u32 val = rdlp(mp, SDMA_CONFIG);
	u64 temp;

	if (mp->shared->extended_rx_coal_limit)
		temp = ((val & 0x02000000) >> 10) | ((val & 0x003fff80) >> 7);
	else
		temp = (val & 0x003fff00) >> 8;

	temp *= 64000000;
	do_div(temp, mp->t_clk);

	return (unsigned int)temp;
}

static void set_rx_coal(struct mv643xx_eth_private *mp, unsigned int usec)
{
	u64 temp;
	u32 val;

	temp = (u64)usec * mp->t_clk;
	temp += 31999999;
	do_div(temp, 64000000);

	val = rdlp(mp, SDMA_CONFIG);
	if (mp->shared->extended_rx_coal_limit) {
		if (temp > 0xffff)
			temp = 0xffff;
		val &= ~0x023fff80;
		val |= (temp & 0x8000) << 10;
		val |= (temp & 0x7fff) << 7;
	} else {
		if (temp > 0x3fff)
			temp = 0x3fff;
		val &= ~0x003fff00;
		val |= (temp & 0x3fff) << 8;
	}
	wrlp(mp, SDMA_CONFIG, val);
}

static unsigned int get_tx_coal(struct mv643xx_eth_private *mp)
{
	u64 temp;

	temp = (rdlp(mp, TX_FIFO_URGENT_THRESHOLD) & 0x3fff0) >> 4;
	temp *= 64000000;
	do_div(temp, mp->t_clk);

	return (unsigned int)temp;
}

static void set_tx_coal(struct mv643xx_eth_private *mp, unsigned int usec)
{
	u64 temp;

	temp = (u64)usec * mp->t_clk;
	temp += 31999999;
	do_div(temp, 64000000);

	if (temp > 0x3fff)
		temp = 0x3fff;

	wrlp(mp, TX_FIFO_URGENT_THRESHOLD, temp << 4);
}


/* ethtool ******************************************************************/
struct mv643xx_eth_stats {
	char stat_string[ETH_GSTRING_LEN];
	int sizeof_stat;
	int netdev_off;
	int mp_off;
};

#define SSTAT(m)						\
	{ #m, FIELD_SIZEOF(struct net_device_stats, m),		\
	  offsetof(struct net_device, stats.m), -1 }

#define MIBSTAT(m)						\
	{ #m, FIELD_SIZEOF(struct mib_counters, m),		\
	  -1, offsetof(struct mv643xx_eth_private, mib_counters.m) }

static const struct mv643xx_eth_stats mv643xx_eth_stats[] = {
	SSTAT(rx_packets),
	SSTAT(tx_packets),
	SSTAT(rx_bytes),
	SSTAT(tx_bytes),
	SSTAT(rx_errors),
	SSTAT(tx_errors),
	SSTAT(rx_dropped),
	SSTAT(tx_dropped),
	MIBSTAT(good_octets_received),
	MIBSTAT(bad_octets_received),
	MIBSTAT(internal_mac_transmit_err),
	MIBSTAT(good_frames_received),
	MIBSTAT(bad_frames_received),
	MIBSTAT(broadcast_frames_received),
	MIBSTAT(multicast_frames_received),
	MIBSTAT(frames_64_octets),
	MIBSTAT(frames_65_to_127_octets),
	MIBSTAT(frames_128_to_255_octets),
	MIBSTAT(frames_256_to_511_octets),
	MIBSTAT(frames_512_to_1023_octets),
	MIBSTAT(frames_1024_to_max_octets),
	MIBSTAT(good_octets_sent),
	MIBSTAT(good_frames_sent),
	MIBSTAT(excessive_collision),
	MIBSTAT(multicast_frames_sent),
	MIBSTAT(broadcast_frames_sent),
	MIBSTAT(unrec_mac_control_received),
	MIBSTAT(fc_sent),
	MIBSTAT(good_fc_received),
	MIBSTAT(bad_fc_received),
	MIBSTAT(undersize_received),
	MIBSTAT(fragments_received),
	MIBSTAT(oversize_received),
	MIBSTAT(jabber_received),
	MIBSTAT(mac_receive_error),
	MIBSTAT(bad_crc_event),
	MIBSTAT(collision),
	MIBSTAT(late_collision),
	MIBSTAT(rx_discard),
	MIBSTAT(rx_overrun),
	MIBSTAT(rx_packets_q[0]),
	MIBSTAT(rx_packets_q[1]),
	MIBSTAT(rx_packets_q[2]),
	MIBSTAT(rx_packets_q[3]),
	MIBSTAT(rx_packets_q[4]),
	MIBSTAT(rx_packets_q[5]),
	MIBSTAT(rx_packets_q[6]),
	MIBSTAT(rx_packets_q[7]),
	MIBSTAT(tx_packets_q[0]),
	MIBSTAT(tx_packets_q[1]),
	MIBSTAT(tx_packets_q[2]),
	MIBSTAT(tx_packets_q[3]),
	MIBSTAT(tx_packets_q[4]),
	MIBSTAT(tx_packets_q[5]),
	MIBSTAT(tx_packets_q[6]),
	MIBSTAT(tx_packets_q[7]),
};

static int
mv643xx_eth_get_settings_phy(struct mv643xx_eth_private *mp,
			     struct ethtool_cmd *cmd)
{
	int err;

	err = phy_read_status(mp->phy);
	if (err == 0)
		err = phy_ethtool_gset(mp->phy, cmd);

	/*
	 * The MAC does not support 1000baseT_Half.
	 */
	cmd->supported &= ~SUPPORTED_1000baseT_Half;
	cmd->advertising &= ~ADVERTISED_1000baseT_Half;

	return err;
}

static int
mv643xx_eth_get_settings_phyless(struct mv643xx_eth_private *mp,
				 struct ethtool_cmd *cmd)
{
	u32 port_status;

	port_status = rdlp(mp, PORT_STATUS);

	cmd->supported = SUPPORTED_MII;
	cmd->advertising = ADVERTISED_MII;
	switch (port_status & PORT_SPEED_MASK) {
	case PORT_SPEED_10:
		ethtool_cmd_speed_set(cmd, SPEED_10);
		break;
	case PORT_SPEED_100:
		ethtool_cmd_speed_set(cmd, SPEED_100);
		break;
	case PORT_SPEED_1000:
		ethtool_cmd_speed_set(cmd, SPEED_1000);
		break;
	default:
		cmd->speed = -1;
		break;
	}
	cmd->duplex = (port_status & FULL_DUPLEX) ? DUPLEX_FULL : DUPLEX_HALF;
	cmd->port = PORT_MII;
	cmd->phy_address = 0;
	cmd->transceiver = XCVR_INTERNAL;
	cmd->autoneg = AUTONEG_DISABLE;
	cmd->maxtxpkt = 1;
	cmd->maxrxpkt = 1;

	return 0;
}

static void
mv643xx_eth_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	wol->supported = 0;
	wol->wolopts = 0;
	if (mp->phy)
		phy_ethtool_get_wol(mp->phy, wol);
}

static int
mv643xx_eth_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int err;

	if (mp->phy == NULL)
		return -EOPNOTSUPP;

	err = phy_ethtool_set_wol(mp->phy, wol);
	/* Given that mv643xx_eth works without the marvell-specific PHY driver,
	 * this debugging hint is useful to have.
	 */
	if (err == -EOPNOTSUPP)
		netdev_info(dev, "The PHY does not support set_wol, was CONFIG_MARVELL_PHY enabled?\n");
	return err;
}

static int
mv643xx_eth_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	if (mp->phy != NULL)
		return mv643xx_eth_get_settings_phy(mp, cmd);
	else
		return mv643xx_eth_get_settings_phyless(mp, cmd);
}

static int
mv643xx_eth_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int ret;

	if (mp->phy == NULL)
		return -EINVAL;

	/*
	 * The MAC does not support 1000baseT_Half.
	 */
	cmd->advertising &= ~ADVERTISED_1000baseT_Half;

	ret = phy_ethtool_sset(mp->phy, cmd);
	if (!ret)
		mv643xx_eth_adjust_link(dev);
	return ret;
}

static void mv643xx_eth_get_drvinfo(struct net_device *dev,
				    struct ethtool_drvinfo *drvinfo)
{
	strlcpy(drvinfo->driver, mv643xx_eth_driver_name,
		sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, mv643xx_eth_driver_version,
		sizeof(drvinfo->version));
	strlcpy(drvinfo->fw_version, "N/A", sizeof(drvinfo->fw_version));
	strlcpy(drvinfo->bus_info, "platform", sizeof(drvinfo->bus_info));
}

static int mv643xx_eth_nway_reset(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	if (mp->phy == NULL)
		return -EINVAL;

	return genphy_restart_aneg(mp->phy);
}

static int
mv643xx_eth_get_coalesce(struct net_device *dev, struct ethtool_coalesce *ec)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	ec->rx_coalesce_usecs = get_rx_coal(mp);
	ec->tx_coalesce_usecs = get_tx_coal(mp);

	return 0;
}

static int
mv643xx_eth_set_coalesce(struct net_device *dev, struct ethtool_coalesce *ec)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	set_rx_coal(mp, ec->rx_coalesce_usecs);
	set_tx_coal(mp, ec->tx_coalesce_usecs);

	return 0;
}

static void
mv643xx_eth_get_ringparam(struct net_device *dev, struct ethtool_ringparam *er)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	er->rx_max_pending = 4096;
	er->tx_max_pending = 4096;

	er->rx_pending = mp->rx_ring_size;
	er->tx_pending = mp->tx_ring_size;
}

static int
mv643xx_eth_set_ringparam(struct net_device *dev, struct ethtool_ringparam *er)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	if (er->rx_mini_pending || er->rx_jumbo_pending)
		return -EINVAL;

	mp->rx_ring_size = er->rx_pending < 4096 ? er->rx_pending : 4096;
	mp->tx_ring_size = clamp_t(unsigned int, er->tx_pending,
				   MV643XX_MAX_SKB_DESCS * 2, 4096);
	if (mp->tx_ring_size != er->tx_pending)
		netdev_warn(dev, "TX queue size set to %u (requested %u)\n",
			    mp->tx_ring_size, er->tx_pending);

	if (netif_running(dev)) {
		mv643xx_eth_stop(dev);
		if (mv643xx_eth_open(dev)) {
			netdev_err(dev,
				   "fatal error on re-opening device after ring param change\n");
			return -ENOMEM;
		}
	}

	return 0;
}

static void
mv643xx_eth_get_channels(struct net_device *dev, struct ethtool_channels *c)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	c->max_rx = 8;
	c->max_tx = 8;
	c->max_other = 0;
	c->max_combined = c->max_rx + c->max_tx;
	c->rx_count = mp->rxq_count;
	c->tx_count = mp->txq_count;
}

static int
mv643xx_eth_set_channels(struct net_device *dev, struct ethtool_channels *c)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	bool was_runnning;

	if (c->rx_count > 8 || c->tx_count > 8 - NAPI_TX_OFFSET ||
	    c->max_other)
		return -EINVAL;

	was_runnning = netif_running(dev);
	if (was_runnning)
		mv643xx_eth_stop(dev);

	mp->rxq_count = c->rx_count;
	mp->txq_count = c->tx_count + NAPI_TX_OFFSET;

	netif_set_real_num_rx_queues(dev, mp->rxq_count);
	netif_set_real_num_tx_queues(dev, mp->txq_count - NAPI_TX_OFFSET);

	if (was_runnning && mv643xx_eth_open(dev)) {
		netdev_err(dev,
			   "fatal error on re-opening device after channels change\n");
		return -ENOMEM;
	}

	return 0;
}

struct vprio_queue {
	int	prio;
	int	queue;
};

static int cmp_queue_inv(const void *a, const void *b)
{
	const struct vprio_queue *pa = a, *pb = b;
	if (pb->queue != pa->queue)
		return pb->queue - pa->queue;
	return pa->prio - pb->prio;
}

static void dump_vlan_rules(struct mv643xx_eth_private *mp,
			    struct vprio_queue *vprio_to_queue)
{
	unsigned int i;
	u32 val;

	val = rdlp(mp, PORT_VPT2P);
	for (i = 0; i < 8; i++) {
		unsigned int queue;

		queue = (val & (0x7 << i * 3)) >> (i * 3);
		vprio_to_queue[i].prio = i;
		vprio_to_queue[i].queue = queue;
	}

	/* sort with higher tx queue first */
	sort(vprio_to_queue, 8, sizeof (vprio_to_queue[0]),
	     cmp_queue_inv, NULL);
}

static unsigned int find_vlan_rule(struct mv643xx_eth_private *mp,
				   unsigned int prio)
{
	struct vprio_queue vprio_to_queue[8];
	unsigned int i;

	/* check if we already have a rule for this vlan */
	dump_vlan_rules(mp, vprio_to_queue);
	for (i = 0; i < ARRAY_SIZE(vprio_to_queue); i++) {
		if (vprio_to_queue[i].prio != prio)
			continue;
		return i;
	}
	/* never reached */
	return 0;
}

static int
mv643xx_eth_get_rxnfc(struct net_device *dev,
		      struct ethtool_rxnfc *info, u32 *rule_locs)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	switch (info->cmd) {
	case ETHTOOL_GRXFH:
		return -ENOTSUPP;
	case ETHTOOL_GRXRINGS:
		info->data = mp->rxq_count;
		break;

	case ETHTOOL_GRXCLSRLCNT:
		info->rule_cnt = 8;
		info->data = RX_CLS_LOC_SPECIAL;
		break;

	case ETHTOOL_GRXCLSRLALL:
	{
		unsigned int i;

		if (info->rule_cnt < 8)
			return -EINVAL;

		info->data = 8;
		info->rule_cnt = 8;

		for (i = 0; i < 8; i++)
			rule_locs[i] = i;

		break;
	}

	case ETHTOOL_GRXCLSRULE:
	{
		struct vprio_queue vprio_to_queue[8], *r;
		struct ethtool_flow_ext *h_ext, *m_ext;
		unsigned int loc;

		loc = info->fs.location;
		if (loc >= ARRAY_SIZE(vprio_to_queue))
			return -EINVAL;

		dump_vlan_rules(mp, vprio_to_queue);
		r = &vprio_to_queue[loc];

		memset(&info->fs, 0, sizeof (info->fs));
		info->fs.flow_type = ETHER_FLOW | FLOW_EXT;
		info->fs.ring_cookie = r->queue;
		info->fs.location = loc;

		m_ext = &info->fs.m_ext;
		m_ext->vlan_tci |= VLAN_PRIO_MASK;

		h_ext = &info->fs.h_ext;
		h_ext->vlan_tci |= r->prio << VLAN_PRIO_SHIFT;

		break;
	}
	}
	return 0;
}

static int
mv643xx_eth_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	switch (info->cmd) {
	case ETHTOOL_SRXFH:
		return -ENOTSUPP;

	case ETHTOOL_SRXCLSRLINS:
	{
		struct ethhdr *m, z;
		struct ethtool_flow_ext *h_ext, *m_ext;
		unsigned int prio;
		unsigned int rule_nr;
		u32 val;

		if ((info->fs.flow_type & (FLOW_MAC_EXT | FLOW_EXT)) !=
		    FLOW_EXT)
			return -EINVAL;

		info->fs.flow_type &= ~FLOW_EXT;
		if (info->fs.flow_type != ETHER_FLOW)
			return -EINVAL;

		if (info->fs.ring_cookie >= mp->rxq_count)
			return -EINVAL;

		if (info->fs.location != RX_CLS_LOC_ANY)
			return -EINVAL;

		/* no mask should be set on ethernet */
		m = &info->fs.m_u.ether_spec;
		memset(&z, 0, sizeof (z));
		if (memcmp(m, &z, sizeof (*m)))
			return -EINVAL;

		/* no mask should be set on ext besides vlan prio */
		m_ext = &info->fs.m_ext;
		if (m_ext->vlan_etype ||
		    m_ext->data[0] ||
		    m_ext->data[1] ||
		    ntohs(m_ext->vlan_tci) != VLAN_PRIO_MASK)
			return -EINVAL;

		/* ok, extract vlan prio */
		h_ext = &info->fs.h_ext;
		prio = (ntohs(h_ext->vlan_tci) & VLAN_PRIO_MASK) >>
			VLAN_PRIO_SHIFT;

		/* update vlan priority table for new rule */
		rule_nr = find_vlan_rule(mp, prio);

		val = rdlp(mp, PORT_VPT2P);
		val |= info->fs.ring_cookie << (prio * 3);
		wrlp(mp, PORT_VPT2P, val);

		info->fs.location = rule_nr;
		break;
	}

	case ETHTOOL_SRXCLSRLDEL:
	{
		struct vprio_queue vprio_to_queue[8], *r;
		u32 val;

		if (info->fs.location >= ARRAY_SIZE(vprio_to_queue))
			return -EINVAL;

		dump_vlan_rules(mp, vprio_to_queue);
		r = &vprio_to_queue[info->fs.location];

		/* update vlan priority table */
		val = rdlp(mp, PORT_VPT2P);
		val &= ~(0x7 << (r->prio * 3));
		wrlp(mp, PORT_VPT2P, val);
		break;
	}
	}

	return 0;
}

static int
mv643xx_eth_set_features(struct net_device *dev, netdev_features_t features)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	bool rx_csum = features & NETIF_F_RXCSUM;

	wrlp(mp, PORT_CONFIG, rx_csum ? 0x02000000 : 0x00000000);

	return 0;
}

static void mv643xx_eth_get_strings(struct net_device *dev,
				    uint32_t stringset, uint8_t *data)
{
	int i;

	if (stringset == ETH_SS_STATS) {
		for (i = 0; i < ARRAY_SIZE(mv643xx_eth_stats); i++) {
			memcpy(data + i * ETH_GSTRING_LEN,
				mv643xx_eth_stats[i].stat_string,
				ETH_GSTRING_LEN);
		}
	}
}

static void mv643xx_eth_get_ethtool_stats(struct net_device *dev,
					  struct ethtool_stats *stats,
					  uint64_t *data)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int i;

	mv643xx_eth_get_stats(dev);
	mib_counters_update(mp);

	for (i = 0; i < ARRAY_SIZE(mv643xx_eth_stats); i++) {
		const struct mv643xx_eth_stats *stat;
		void *p;

		stat = mv643xx_eth_stats + i;

		if (stat->netdev_off >= 0)
			p = ((void *)mp->dev) + stat->netdev_off;
		else
			p = ((void *)mp) + stat->mp_off;

		data[i] = (stat->sizeof_stat == 8) ?
				*(uint64_t *)p : *(uint32_t *)p;
	}
}

static int mv643xx_eth_get_sset_count(struct net_device *dev, int sset)
{
	if (sset == ETH_SS_STATS)
		return ARRAY_SIZE(mv643xx_eth_stats);

	return -EOPNOTSUPP;
}

static const struct ethtool_ops mv643xx_eth_ethtool_ops = {
	.get_settings		= mv643xx_eth_get_settings,
	.set_settings		= mv643xx_eth_set_settings,
	.get_drvinfo		= mv643xx_eth_get_drvinfo,
	.nway_reset		= mv643xx_eth_nway_reset,
	.get_link		= ethtool_op_get_link,
	.get_coalesce		= mv643xx_eth_get_coalesce,
	.set_coalesce		= mv643xx_eth_set_coalesce,
	.get_ringparam		= mv643xx_eth_get_ringparam,
	.set_ringparam		= mv643xx_eth_set_ringparam,
	.get_strings		= mv643xx_eth_get_strings,
	.get_ethtool_stats	= mv643xx_eth_get_ethtool_stats,
	.get_sset_count		= mv643xx_eth_get_sset_count,
	.get_ts_info		= ethtool_op_get_ts_info,
	.get_wol                = mv643xx_eth_get_wol,
	.set_wol                = mv643xx_eth_set_wol,
	.get_channels		= mv643xx_eth_get_channels,
	.set_channels		= mv643xx_eth_set_channels,
	.get_rxnfc		= mv643xx_eth_get_rxnfc,
	.set_rxnfc		= mv643xx_eth_set_rxnfc,
};


/* address handling *********************************************************/
static void uc_addr_get(struct mv643xx_eth_private *mp, unsigned char *addr)
{
	unsigned int mac_h = rdlp(mp, MAC_ADDR_HIGH);
	unsigned int mac_l = rdlp(mp, MAC_ADDR_LOW);

	addr[0] = (mac_h >> 24) & 0xff;
	addr[1] = (mac_h >> 16) & 0xff;
	addr[2] = (mac_h >> 8) & 0xff;
	addr[3] = mac_h & 0xff;
	addr[4] = (mac_l >> 8) & 0xff;
	addr[5] = mac_l & 0xff;
}

static void uc_addr_set(struct mv643xx_eth_private *mp, unsigned char *addr)
{
	wrlp(mp, MAC_ADDR_HIGH,
		(addr[0] << 24) | (addr[1] << 16) | (addr[2] << 8) | addr[3]);
	wrlp(mp, MAC_ADDR_LOW, (addr[4] << 8) | addr[5]);
}

static u32 uc_addr_filter_mask(struct net_device *dev)
{
	struct netdev_hw_addr *ha;
	u32 nibbles;

	if (dev->flags & IFF_PROMISC)
		return 0;

	nibbles = 1 << (dev->dev_addr[5] & 0x0f);
	netdev_for_each_uc_addr(ha, dev) {
		if (memcmp(dev->dev_addr, ha->addr, 5))
			return 0;
		if ((dev->dev_addr[5] ^ ha->addr[5]) & 0xf0)
			return 0;

		nibbles |= 1 << (ha->addr[5] & 0x0f);
	}

	return nibbles;
}

static void mv643xx_eth_program_unicast_filter(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	u32 port_config;
	u32 nibbles;
	int i;

	uc_addr_set(mp, dev->dev_addr);

	port_config = rdlp(mp, PORT_CONFIG) & ~UNICAST_PROMISCUOUS_MODE;

	nibbles = uc_addr_filter_mask(dev);
	if (!nibbles) {
		port_config |= UNICAST_PROMISCUOUS_MODE;
		nibbles = 0xffff;
	}

	for (i = 0; i < 16; i += 4) {
		int off = UNICAST_TABLE(mp->port_num) + i;
		u32 v;

		v = 0;
		if (nibbles & 1)
			v |= 0x00000001;
		if (nibbles & 2)
			v |= 0x00000100;
		if (nibbles & 4)
			v |= 0x00010000;
		if (nibbles & 8)
			v |= 0x01000000;
		nibbles >>= 4;

		wrl(mp, off, v);
	}

	wrlp(mp, PORT_CONFIG, port_config);
}

static int addr_crc(unsigned char *addr)
{
	int crc = 0;
	int i;

	for (i = 0; i < 6; i++) {
		int j;

		crc = (crc ^ addr[i]) << 8;
		for (j = 7; j >= 0; j--) {
			if (crc & (0x100 << j))
				crc ^= 0x107 << j;
		}
	}

	return crc;
}

static void mv643xx_eth_program_multicast_filter(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	u32 *mc_spec;
	u32 *mc_other;
	struct netdev_hw_addr *ha;
	int i;

	if (dev->flags & (IFF_PROMISC | IFF_ALLMULTI))
		goto promiscuous;

	/* Allocate both mc_spec and mc_other tables */
	mc_spec = kcalloc(128, sizeof(u32), GFP_ATOMIC);
	if (!mc_spec)
		goto promiscuous;
	mc_other = &mc_spec[64];

	netdev_for_each_mc_addr(ha, dev) {
		u8 *a = ha->addr;
		u32 *table;
		u8 entry;

		if (memcmp(a, "\x01\x00\x5e\x00\x00", 5) == 0) {
			table = mc_spec;
			entry = a[5];
		} else {
			table = mc_other;
			entry = addr_crc(a);
		}

		table[entry >> 2] |= 1 << (8 * (entry & 3));
	}

	for (i = 0; i < 64; i++) {
		wrl(mp, SPECIAL_MCAST_TABLE(mp->port_num) + i * sizeof(u32),
		    mc_spec[i]);
		wrl(mp, OTHER_MCAST_TABLE(mp->port_num) + i * sizeof(u32),
		    mc_other[i]);
	}

	kfree(mc_spec);
	return;

promiscuous:
	for (i = 0; i < 64; i++) {
		wrl(mp, SPECIAL_MCAST_TABLE(mp->port_num) + i * sizeof(u32),
		    0x01010101u);
		wrl(mp, OTHER_MCAST_TABLE(mp->port_num) + i * sizeof(u32),
		    0x01010101u);
	}
}

static void mv643xx_eth_set_rx_mode(struct net_device *dev)
{
	mv643xx_eth_program_unicast_filter(dev);
	mv643xx_eth_program_multicast_filter(dev);
}

static int mv643xx_eth_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = addr;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, sa->sa_data, ETH_ALEN);

	netif_addr_lock_bh(dev);
	mv643xx_eth_program_unicast_filter(dev);
	netif_addr_unlock_bh(dev);

	return 0;
}

/* rx/tx queue initialisation ***********************************************/
static int rxq_init(struct mv643xx_eth_private *mp, int index)
{
	struct rx_queue *rxq = mp->rxq + index;
	struct rx_desc *rx_desc;
	int size;
	int i;

	rxq->index = index;

	rxq->rx_ring_size = mp->rx_ring_size;
	rxq->rx_curr_desc = 0;

	size = rxq->rx_ring_size * sizeof(struct rx_desc);

	if (index == 0 && size <= mp->rx_desc_sram_size) {
		rxq->rx_desc_area = ioremap(mp->rx_desc_sram_addr,
						mp->rx_desc_sram_size);
		rxq->rx_desc_dma = mp->rx_desc_sram_addr;
	} else {
		rxq->rx_desc_area = dma_alloc_coherent(mp->dev->dev.parent,
						       size, &rxq->rx_desc_dma,
						       GFP_KERNEL);
	}

	if (rxq->rx_desc_area == NULL) {
		netdev_err(mp->dev,
			   "can't allocate rx ring (%d bytes)\n", size);
		goto out;
	}
	memset(rxq->rx_desc_area, 0, size);

	rxq->rx_desc_area_size = size;

	rx_desc = rxq->rx_desc_area;
	for (i = 0; i < rxq->rx_ring_size; i++) {
		int ret, nexti;

                ret = rx_desc_refill(mp, &rx_desc[i], false);
		if (ret)
			goto out_free;

		nexti = i + 1;
		if (nexti == rxq->rx_ring_size)
			nexti = 0;

		rx_desc[i].next_desc_ptr = rxq->rx_desc_dma +
					nexti * sizeof(struct rx_desc);
	}

	return 0;


out_free:
	for (i = 0; i < rxq->rx_ring_size; i++) {
		if (!rx_desc[i].cookie)
			break;
		mv643xx_eth_frag_free(mp, (void *)rx_desc[i].cookie);
	}

	if (index == 0 && size <= mp->rx_desc_sram_size)
		iounmap(rxq->rx_desc_area);
	else
		dma_free_coherent(mp->dev->dev.parent, size,
				  rxq->rx_desc_area,
				  rxq->rx_desc_dma);

out:
	return -ENOMEM;
}

static void rxq_deinit(struct rx_queue *rxq)
{
	struct mv643xx_eth_private *mp = rxq_to_mp(rxq);
	int i;

	rxq_disable(rxq);

	for (i = 0; i < rxq->rx_ring_size; i++)
		mv643xx_eth_frag_free(mp, (void *)rxq->rx_desc_area[i].cookie);

	if (rxq->index == 0 &&
	    rxq->rx_desc_area_size <= mp->rx_desc_sram_size)
		iounmap(rxq->rx_desc_area);
	else
		dma_free_coherent(mp->dev->dev.parent, rxq->rx_desc_area_size,
				  rxq->rx_desc_area, rxq->rx_desc_dma);
}

static int txq_init(struct mv643xx_eth_private *mp, int index)
{
	struct tx_queue *txq = mp->txq + index;
	struct tx_desc *tx_desc;
	int size;
	int ret;
	int i;

	txq->index = index;
	txq->tx_ring_size = mp->tx_ring_size;

	/* A queue must always have room for at least one skb.
	 * Therefore, stop the queue when the free entries reaches
	 * the maximum number of descriptors per skb.
	 */
	txq->tx_stop_threshold = txq->tx_ring_size - MV643XX_MAX_SKB_DESCS;
	txq->tx_wake_threshold = txq->tx_stop_threshold / 2;

	txq->tx_desc_count = 0;
	txq->tx_curr_desc = 0;
	txq->tx_used_desc = 0;

	size = txq->tx_ring_size * sizeof(struct tx_desc);

	if (index == 0 && size <= mp->tx_desc_sram_size) {
		txq->tx_desc_area = ioremap(mp->tx_desc_sram_addr,
						mp->tx_desc_sram_size);
		txq->tx_desc_dma = mp->tx_desc_sram_addr;
	} else {
		txq->tx_desc_area = dma_alloc_coherent(mp->dev->dev.parent,
						       size, &txq->tx_desc_dma,
						       GFP_KERNEL);
	}

	if (txq->tx_desc_area == NULL) {
		netdev_err(mp->dev,
			   "can't allocate tx ring (%d bytes)\n", size);
		return -ENOMEM;
	}
	memset(txq->tx_desc_area, 0, size);

	txq->tx_desc_area_size = size;

	tx_desc = txq->tx_desc_area;
	for (i = 0; i < txq->tx_ring_size; i++) {
		struct tx_desc *txd = tx_desc + i;
		int nexti;

		nexti = i + 1;
		if (nexti == txq->tx_ring_size)
			nexti = 0;

		txd->cmd_sts = 0;
		txd->next_desc_ptr = txq->tx_desc_dma +
					nexti * sizeof(struct tx_desc);
	}

	txq->tx_desc_mapping = kcalloc(txq->tx_ring_size, sizeof(char),
				       GFP_KERNEL);
	if (!txq->tx_desc_mapping) {
		ret = -ENOMEM;
		goto err_free_desc_area;
	}

	/* Allocate DMA buffers for TSO MAC/IP/TCP headers */
	txq->tso_hdrs = dma_alloc_coherent(mp->dev->dev.parent,
					   txq->tx_ring_size * TSO_HEADER_SIZE,
					   &txq->tso_hdrs_dma, GFP_KERNEL);
	if (txq->tso_hdrs == NULL) {
		ret = -ENOMEM;
		goto err_free_desc_mapping;
	}
	skb_queue_head_init(&txq->tx_skb);

	return 0;

err_free_desc_mapping:
	kfree(txq->tx_desc_mapping);
err_free_desc_area:
	if (index == 0 && size <= mp->tx_desc_sram_size)
		iounmap(txq->tx_desc_area);
	else
		dma_free_coherent(mp->dev->dev.parent, txq->tx_desc_area_size,
				  txq->tx_desc_area, txq->tx_desc_dma);
	return ret;
}

static void txq_deinit(struct tx_queue *txq)
{
	struct mv643xx_eth_private *mp = txq_to_mp(txq);

	txq_disable(txq);
	txq_reclaim(txq, txq->tx_ring_size, 1);

	BUG_ON(txq->tx_used_desc != txq->tx_curr_desc);

	if (txq->index == 0 &&
	    txq->tx_desc_area_size <= mp->tx_desc_sram_size)
		iounmap(txq->tx_desc_area);
	else
		dma_free_coherent(mp->dev->dev.parent, txq->tx_desc_area_size,
				  txq->tx_desc_area, txq->tx_desc_dma);
	kfree(txq->tx_desc_mapping);

	if (txq->tso_hdrs)
		dma_free_coherent(mp->dev->dev.parent,
				  txq->tx_ring_size * TSO_HEADER_SIZE,
				  txq->tso_hdrs, txq->tso_hdrs_dma);
}


/* netdev ops and related ***************************************************/
static int mv643xx_eth_collect_events(struct mv643xx_eth_private *mp)
{
	u32 int_cause;
	u32 int_cause_ext;

	int_cause = rdlp(mp, INT_CAUSE) & mp->int_mask;
	if (int_cause == 0)
		return 0;

	int_cause_ext = 0;
	if (int_cause & INT_EXT) {
		int_cause &= ~INT_EXT;
		int_cause_ext = rdlp(mp, INT_CAUSE_EXT);
	}

	if (int_cause) {
		wrlp(mp, INT_CAUSE, ~int_cause);
		mp->work_tx_end |= ((int_cause & INT_TX_END) >> 19) &
				~(rdlp(mp, TXQ_COMMAND) & 0xff);
#ifdef CONFIG_MV643XX_ETH_FBX_FF
		mp->work_tx_end &= ~INT_TX_END_0;
#endif
		mp->work_rx |= (int_cause & INT_RX) >> 2;
	}

	int_cause_ext &= INT_EXT_LINK_PHY | INT_EXT_TX;
	if (int_cause_ext) {
		wrlp(mp, INT_CAUSE_EXT, ~int_cause_ext);
		if (int_cause_ext & INT_EXT_LINK_PHY)
			mp->work_link = 1;
		mp->work_tx |= int_cause_ext & INT_EXT_TX;
#ifdef CONFIG_MV643XX_ETH_FBX_FF
		mp->work_tx &= ~INT_EXT_TX_0;
#endif
	}

	return 1;
}

static irqreturn_t mv643xx_eth_irq(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	if (unlikely(!mv643xx_eth_collect_events(mp)))
		return IRQ_NONE;

	wrlp(mp, INT_MASK, 0);
	napi_schedule(&mp->napi);

	return IRQ_HANDLED;
}

static void handle_link_event(struct mv643xx_eth_private *mp)
{
	struct net_device *dev = mp->dev;
	u32 port_status;
	int speed;
	int duplex;
	int fc;

	port_status = rdlp(mp, PORT_STATUS);
	if (!(port_status & LINK_UP)) {
		if (netif_carrier_ok(dev)) {
			int i;

			netdev_info(dev, "link down\n");

			netif_carrier_off(dev);

			for (i = 0; i < mp->txq_count; i++) {
				struct tx_queue *txq = mp->txq + i;

				txq_reclaim(txq, txq->tx_ring_size, 1);
				txq_reset_hw_ptr(txq);
			}
		}
		return;
	}

	switch (port_status & PORT_SPEED_MASK) {
	case PORT_SPEED_10:
		speed = 10;
		break;
	case PORT_SPEED_100:
		speed = 100;
		break;
	case PORT_SPEED_1000:
		speed = 1000;
		break;
	default:
		speed = -1;
		break;
	}
	duplex = (port_status & FULL_DUPLEX) ? 1 : 0;
	fc = (port_status & FLOW_CONTROL_ENABLED) ? 1 : 0;

	netdev_info(dev, "link up, %d Mb/s, %s duplex, flow control %sabled\n",
		    speed, duplex ? "full" : "half", fc ? "en" : "dis");

	if (!netif_carrier_ok(dev))
		netif_carrier_on(dev);
}

static int mv643xx_eth_poll(struct napi_struct *napi, int budget)
{
	struct mv643xx_eth_private *mp;
	int work_done;

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	ff.jiffies = jiffies;
#endif

	mp = container_of(napi, struct mv643xx_eth_private, napi);

	work_done = 0;
	while (work_done < budget) {
		u8 queue_mask;
		int queue;
		int work_tbd;

		if (mp->work_link) {
			mp->work_link = 0;
			handle_link_event(mp);
			work_done++;
			continue;
		}

		queue_mask = mp->work_tx | mp->work_tx_end | mp->work_rx;

		if (!queue_mask) {
			if (mv643xx_eth_collect_events(mp))
				continue;
			break;
		}

		queue = fls(queue_mask) - 1;
		queue_mask = 1 << queue;

		work_tbd = budget - work_done;
		if (work_tbd > 16)
			work_tbd = 16;

		if (mp->work_tx_end & queue_mask) {
			txq_kick(mp->txq + queue);
		} else if (mp->work_tx & queue_mask) {
			work_done += txq_reclaim(mp->txq + queue, work_tbd, 0);
			txq_maybe_wake(mp->txq + queue);
		} else if (mp->work_rx & queue_mask) {
			work_done += rxq_process(mp->rxq + queue, work_tbd);
		} else {
			BUG();
		}
	}

	if (work_done < budget) {
		napi_complete(napi);
		wrlp(mp, INT_MASK, mp->int_mask);
	}

	return work_done;
}

static void port_start(struct mv643xx_eth_private *mp)
{
	u32 pscr;
	int i;

	/*
	 * Perform PHY reset, if there is a PHY.
	 */
	if (mp->phy != NULL) {
		struct ethtool_cmd cmd;

		mv643xx_eth_get_settings(mp->dev, &cmd);
		phy_init_hw(mp->phy);
		mv643xx_eth_set_settings(mp->dev, &cmd);
		phy_start(mp->phy);
	}

	/*
	 * Configure basic link parameters.
	 */
	pscr = rdlp(mp, PORT_SERIAL_CONTROL);

	pscr |= SERIAL_PORT_ENABLE;
	wrlp(mp, PORT_SERIAL_CONTROL, pscr);

	pscr |= DO_NOT_FORCE_LINK_FAIL;
	if (mp->phy == NULL)
		pscr |= FORCE_LINK_PASS;
	wrlp(mp, PORT_SERIAL_CONTROL, pscr);

	/*
	 * Configure TX path and queues.
	 */
	tx_set_rate(mp, 1000000000, 16777216);
	for (i = 0; i < mp->txq_count; i++) {
		struct tx_queue *txq = mp->txq + i;

		txq_reset_hw_ptr(txq);
		txq_set_rate(txq, 1000000000, 16777216);
		txq_set_fixed_prio_mode(txq);
	}

	/*
	 * Receive all unmatched unicast, TCP, UDP, BPDU and broadcast
	 * frames to RX queue #0, and include the pseudo-header when
	 * calculating receive checksums.
	 */
	mv643xx_eth_set_features(mp->dev, mp->dev->features);

	/*
	 * Treat BPDUs as normal multicasts, and disable partition mode.
	 */
	wrlp(mp, PORT_CONFIG_EXT, 0x00000000);

	/*
	 * Add configured unicast addresses to address filter table.
	 */
	mv643xx_eth_program_unicast_filter(mp->dev);

	/*
	 * Enable the receive queues.
	 */
	for (i = 0; i < mp->rxq_count; i++) {
		struct rx_queue *rxq = mp->rxq + i;
		u32 addr;

		addr = (u32)rxq->rx_desc_dma;
		addr += rxq->rx_curr_desc * sizeof(struct rx_desc);
		wrlp(mp, RXQ_CURRENT_DESC_PTR(i), addr);

		rxq_enable(rxq);
	}
}

static void mv643xx_eth_recalc_frag_size(struct mv643xx_eth_private *mp)
{
	/*
	 * Reserve 2+14 bytes for an ethernet header (the hardware
	 * automatically prepends 2 bytes of dummy data to each
	 * received packet), 16 bytes for up to four VLAN tags, and
	 * 4 bytes for the trailing FCS -- 36 bytes total.
	 */
	mp->pkt_size = mp->dev->mtu + 36;

	/*
	 * Make sure that the buffer size is a multiple of 8 bytes, as
	 * the lower three bits of the receive descriptor's buffer
	 * size field are ignored by the hardware.
	 */
	BUILD_BUG_ON(SMP_CACHE_BYTES < 8);

	/*
	 * add NET_SKB_PAD per build_skb() requirement, make sure we
	 * have room to align data to cache size after reserving
	 */
	mp->frag_size = mp->pkt_size + RX_OFFSET;

	/*
	 * per build_skb() requirement
	 */
	mp->frag_size = (SKB_DATA_ALIGN(mp->frag_size) +
			 SKB_DATA_ALIGN(sizeof (struct skb_shared_info)));
}

static int mv643xx_eth_open(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int err;
	int i;

	wrlp(mp, INT_CAUSE, 0);
	wrlp(mp, INT_CAUSE_EXT, 0);
	rdlp(mp, INT_CAUSE_EXT);

	err = request_irq(dev->irq, mv643xx_eth_irq,
			  IRQF_SHARED, dev->name, dev);
	if (err) {
		netdev_err(dev, "can't assign irq\n");
		return -EAGAIN;
	}

	mv643xx_eth_recalc_frag_size(mp);

	napi_enable(&mp->napi);

	mp->int_mask = INT_EXT;

	for (i = 0; i < mp->rxq_count; i++) {
		err = rxq_init(mp, i);
		if (err) {
			while (--i >= 0)
				rxq_deinit(mp->rxq + i);
			goto out;
		}

		mp->int_mask |= INT_RX_0 << i;
	}

	for (i = 0; i < mp->txq_count; i++) {
		err = txq_init(mp, i);
		if (err) {
			while (--i >= 0)
				txq_deinit(mp->txq + i);
			goto out_free;
		}

#ifdef CONFIG_MV643XX_ETH_FBX_FF
		if (i != 0)
		mp->int_mask |= INT_TX_END_0 << i;
#else
		mp->int_mask |= INT_TX_END_0 << i;
#endif
	}

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	mp->ff_txq = &mp->txq[0];
#endif

	add_timer(&mp->mib_counters_timer);
	port_start(mp);

	wrlp(mp, INT_MASK_EXT, INT_EXT_LINK_PHY | INT_EXT_TX);
	wrlp(mp, INT_MASK, mp->int_mask);

	mp_by_unit[mp->shared->unit] = mp;
	return 0;


out_free:
	for (i = 0; i < mp->rxq_count; i++)
		rxq_deinit(mp->rxq + i);
out:
	free_irq(dev->irq, dev);

	return err;
}

static void port_reset(struct mv643xx_eth_private *mp)
{
	unsigned int data;
	int i;

	for (i = 0; i < mp->rxq_count; i++)
		rxq_disable(mp->rxq + i);
	for (i = 0; i < mp->txq_count; i++)
		txq_disable(mp->txq + i);

	while (1) {
		u32 ps = rdlp(mp, PORT_STATUS);

		if ((ps & (TX_IN_PROGRESS | TX_FIFO_EMPTY)) == TX_FIFO_EMPTY)
			break;
		udelay(10);
	}

	/* Reset the Enable bit in the Configuration Register */
	data = rdlp(mp, PORT_SERIAL_CONTROL);
	data &= ~(SERIAL_PORT_ENABLE		|
		  DO_NOT_FORCE_LINK_FAIL	|
		  FORCE_LINK_PASS);
	wrlp(mp, PORT_SERIAL_CONTROL, data);
}

static int mv643xx_eth_stop(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int i;

	mp_by_unit[mp->shared->unit] = NULL;

	wrlp(mp, INT_MASK_EXT, 0x00000000);
	wrlp(mp, INT_MASK, 0x00000000);
	rdlp(mp, INT_MASK);

	napi_disable(&mp->napi);

	netif_carrier_off(dev);
	if (mp->phy)
		phy_stop(mp->phy);
	free_irq(dev->irq, dev);

	port_reset(mp);
	mv643xx_eth_get_stats(dev);
	mib_counters_update(mp);
	del_timer_sync(&mp->mib_counters_timer);

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	mp->ff_txq = NULL;
#endif

	for (i = 0; i < mp->rxq_count; i++)
		rxq_deinit(mp->rxq + i);
	for (i = 0; i < mp->txq_count; i++)
		txq_deinit(mp->txq + i);

	return 0;
}

static int mv643xx_eth_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	int ret;

	if (mp->phy != NULL) {
	ret = phy_mii_ioctl(mp->phy, ifr, cmd);
	if (!ret)
		mv643xx_eth_adjust_link(dev);
	} else {
		struct mii_if_info mii;

		mii.dev = dev;
		mii.mdio_read = mii_bus_read;
		mii.mdio_write = mii_bus_write;
		mii.phy_id = 0;
		mii.phy_id_mask = 0x3f;
		mii.reg_num_mask = 0x1f;
		return generic_mii_ioctl(&mii, if_mii(ifr), cmd, NULL);
	}
	return -ENOTSUPP;
}

static int mv643xx_eth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	if (new_mtu < 64 || new_mtu > 9500)
		return -EINVAL;

	dev->mtu = new_mtu;
	mv643xx_eth_recalc_frag_size(mp);
	tx_set_rate(mp, 1000000000, 16777216);

	if (!netif_running(dev))
		return 0;

	/*
	 * Stop and then re-open the interface. This will allocate RX
	 * skbs of the new MTU.
	 * There is a possible danger that the open will not succeed,
	 * due to memory being full.
	 */
	mv643xx_eth_stop(dev);
	if (mv643xx_eth_open(dev)) {
		netdev_err(dev,
			   "fatal error on re-opening device after MTU change\n");
	}

	return 0;
}

static void tx_timeout_task(struct work_struct *ugly)
{
	struct mv643xx_eth_private *mp;

	mp = container_of(ugly, struct mv643xx_eth_private, tx_timeout_task);
	if (netif_running(mp->dev)) {
		netif_tx_stop_all_queues(mp->dev);
		port_reset(mp);
		port_start(mp);
		netif_tx_wake_all_queues(mp->dev);
	}
}

static void mv643xx_eth_tx_timeout(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	netdev_info(dev, "tx timeout\n");

	schedule_work(&mp->tx_timeout_task);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void mv643xx_eth_netpoll(struct net_device *dev)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);

	wrlp(mp, INT_MASK, 0x00000000);
	rdlp(mp, INT_MASK);

	mv643xx_eth_irq(dev->irq, dev);

	wrlp(mp, INT_MASK, mp->int_mask);
}
#endif


/* platform glue ************************************************************/
static void
mv643xx_eth_conf_mbus_windows(struct mv643xx_eth_shared_private *msp,
			      const struct mbus_dram_target_info *dram)
{
	void __iomem *base = msp->base;
	u32 win_enable;
	u32 win_protect;
	int i;

	for (i = 0; i < 6; i++) {
		writel(0, base + WINDOW_BASE(i));
		writel(0, base + WINDOW_SIZE(i));
		if (i < 4)
			writel(0, base + WINDOW_REMAP_HIGH(i));
	}

	win_enable = 0x3f;
	win_protect = 0;

	for (i = 0; i < dram->num_cs; i++) {
		const struct mbus_dram_window *cs = dram->cs + i;

		writel((cs->base & 0xffff0000) |
			(cs->mbus_attr << 8) |
			dram->mbus_dram_target_id, base + WINDOW_BASE(i));
		writel((cs->size - 1) & 0xffff0000, base + WINDOW_SIZE(i));

		win_enable &= ~(1 << i);
		win_protect |= 3 << (2 * i);
	}

	writel(win_enable, base + WINDOW_BAR_ENABLE);
	msp->win_protect = win_protect;
}

static void infer_hw_params(struct mv643xx_eth_shared_private *msp)
{
	/*
	 * Check whether we have a 14-bit coal limit field in bits
	 * [21:8], or a 16-bit coal limit in bits [25,21:7] of the
	 * SDMA config register.
	 */
	writel(0x02000000, msp->base + 0x0400 + SDMA_CONFIG);
	if (readl(msp->base + 0x0400 + SDMA_CONFIG) & 0x02000000)
		msp->extended_rx_coal_limit = 1;
	else
		msp->extended_rx_coal_limit = 0;

	/*
	 * Check whether the MAC supports TX rate control, and if
	 * yes, whether its associated registers are in the old or
	 * the new place.
	 */
	writel(1, msp->base + 0x0400 + TX_BW_MTU_MOVED);
	if (readl(msp->base + 0x0400 + TX_BW_MTU_MOVED) & 1) {
		msp->tx_bw_control = TX_BW_CONTROL_NEW_LAYOUT;
	} else {
		writel(7, msp->base + 0x0400 + TX_BW_RATE);
		if (readl(msp->base + 0x0400 + TX_BW_RATE) & 7)
			msp->tx_bw_control = TX_BW_CONTROL_OLD_LAYOUT;
		else
			msp->tx_bw_control = TX_BW_CONTROL_ABSENT;
	}
}

#if defined(CONFIG_OF)
static const struct of_device_id mv643xx_eth_shared_ids[] = {
	{ .compatible = "marvell,orion-eth", },
	{ .compatible = "marvell,kirkwood-eth", },
	{ }
};
MODULE_DEVICE_TABLE(of, mv643xx_eth_shared_ids);
#endif

#if defined(CONFIG_OF) && !defined(CONFIG_MV64X60)
#define mv643xx_eth_property(_np, _name, _v)				\
	do {								\
		u32 tmp;						\
		if (!of_property_read_u32(_np, "marvell," _name, &tmp))	\
			_v = tmp;					\
	} while (0)

static struct platform_device *port_platdev[3];

static int mv643xx_eth_shared_of_add_port(struct platform_device *pdev,
					  struct device_node *pnp)
{
	struct platform_device *ppdev;
	struct mv643xx_eth_platform_data ppd;
	struct resource res;
	const char *mac_addr;
	int ret;
	int dev_num = 0;

	memset(&ppd, 0, sizeof(ppd));
	ppd.shared = pdev;

	memset(&res, 0, sizeof(res));
	if (!of_irq_to_resource(pnp, 0, &res)) {
		dev_err(&pdev->dev, "missing interrupt on %s\n", pnp->name);
		return -EINVAL;
	}

	if (of_property_read_u32(pnp, "reg", &ppd.port_number)) {
		dev_err(&pdev->dev, "missing reg property on %s\n", pnp->name);
		return -EINVAL;
	}

	if (ppd.port_number >= 3) {
		dev_err(&pdev->dev, "invalid reg property on %s\n", pnp->name);
		return -EINVAL;
	}

	while (dev_num < 3 && port_platdev[dev_num])
		dev_num++;

	if (dev_num == 3) {
		dev_err(&pdev->dev, "too many ports registered\n");
		return -EINVAL;
	}

	mac_addr = of_get_mac_address(pnp);
	if (mac_addr)
		memcpy(ppd.mac_addr, mac_addr, ETH_ALEN);

	mv643xx_eth_property(pnp, "tx-queue-size", ppd.tx_queue_size);
	mv643xx_eth_property(pnp, "tx-sram-addr", ppd.tx_sram_addr);
	mv643xx_eth_property(pnp, "tx-sram-size", ppd.tx_sram_size);
	mv643xx_eth_property(pnp, "rx-queue-size", ppd.rx_queue_size);
	mv643xx_eth_property(pnp, "rx-sram-addr", ppd.rx_sram_addr);
	mv643xx_eth_property(pnp, "rx-sram-size", ppd.rx_sram_size);

	ppd.phy_node = of_parse_phandle(pnp, "phy-handle", 0);
	if (!ppd.phy_node) {
		ppd.phy_addr = MV643XX_ETH_PHY_NONE;
		of_property_read_u32(pnp, "speed", &ppd.speed);
		of_property_read_u32(pnp, "duplex", &ppd.duplex);
	}

	ppdev = platform_device_alloc(MV643XX_ETH_NAME, dev_num);
	if (!ppdev)
		return -ENOMEM;
	ppdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	ppdev->dev.of_node = pnp;

	ret = platform_device_add_resources(ppdev, &res, 1);
	if (ret)
		goto port_err;

	ret = platform_device_add_data(ppdev, &ppd, sizeof(ppd));
	if (ret)
		goto port_err;

	ret = platform_device_add(ppdev);
	if (ret)
		goto port_err;

	port_platdev[dev_num] = ppdev;

	return 0;

port_err:
	platform_device_put(ppdev);
	return ret;
}

static int mv643xx_eth_shared_of_probe(struct platform_device *pdev)
{
	struct mv643xx_eth_shared_platform_data *pd;
	struct device_node *pnp, *np = pdev->dev.of_node;
	int ret;

	/* bail out if not registered from DT */
	if (!np)
		return 0;

	pd = devm_kzalloc(&pdev->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pdev->dev.platform_data = pd;

	mv643xx_eth_property(np, "tx-checksum-limit", pd->tx_csum_limit);
	mv643xx_eth_property(np, "unit", pd->unit);

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	if (!of_property_read_u32(np, "fbx,ff-model", &ff_mode))
		ff_init(&pdev->dev);
#endif

	for_each_available_child_of_node(np, pnp) {
		ret = mv643xx_eth_shared_of_add_port(pdev, pnp);
		if (ret) {
			of_node_put(pnp);
			return ret;
		}
	}
	return 0;
}

static void mv643xx_eth_shared_of_remove(void)
{
	int n;

	for (n = 0; n < 3; n++) {
		platform_device_del(port_platdev[n]);
		port_platdev[n] = NULL;
	}
}
#else
static inline int mv643xx_eth_shared_of_probe(struct platform_device *pdev)
{
	return 0;
}

static inline void mv643xx_eth_shared_of_remove(void)
{
}
#endif

static int mv643xx_eth_shared_probe(struct platform_device *pdev)
{
	static int mv643xx_eth_version_printed;
	struct mv643xx_eth_shared_platform_data *pd;
	struct mv643xx_eth_shared_private *msp;
	const struct mbus_dram_target_info *dram;
	struct resource *res;
	int ret;

	if (!mv643xx_eth_version_printed++)
		pr_notice("MV-643xx 10/100/1000 ethernet driver version %s\n",
			  mv643xx_eth_driver_version);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -EINVAL;

	msp = devm_kzalloc(&pdev->dev, sizeof(*msp), GFP_KERNEL);
	if (msp == NULL)
		return -ENOMEM;
	platform_set_drvdata(pdev, msp);

	msp->base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (msp->base == NULL)
		return -ENOMEM;

	msp->clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(msp->clk))
		clk_prepare_enable(msp->clk);

	/*
	 * (Re-)program MBUS remapping windows if we are asked to.
	 */
	dram = mv_mbus_dram_info();
	if (dram)
		mv643xx_eth_conf_mbus_windows(msp, dram);

	ret = mv643xx_eth_shared_of_probe(pdev);
	if (ret)
		goto err_put_clk;
	pd = dev_get_platdata(&pdev->dev);

	msp->unit = (pd ? pd->unit : 0);
	msp->tx_csum_limit = (pd != NULL && pd->tx_csum_limit) ?
					pd->tx_csum_limit : 9 * 1024;
	infer_hw_params(msp);

	return 0;

err_put_clk:
	if (!IS_ERR(msp->clk))
		clk_disable_unprepare(msp->clk);
	return ret;
}

static int mv643xx_eth_shared_remove(struct platform_device *pdev)
{
	struct mv643xx_eth_shared_private *msp = platform_get_drvdata(pdev);

	mv643xx_eth_shared_of_remove();
	if (!IS_ERR(msp->clk))
		clk_disable_unprepare(msp->clk);
	return 0;
}

static struct platform_driver mv643xx_eth_shared_driver = {
	.probe		= mv643xx_eth_shared_probe,
	.remove		= mv643xx_eth_shared_remove,
	.driver = {
		.name	= MV643XX_ETH_SHARED_NAME,
		.of_match_table = of_match_ptr(mv643xx_eth_shared_ids),
	},
};

static void phy_addr_set(struct mv643xx_eth_private *mp, int phy_addr)
{
	int addr_shift = 5 * mp->port_num;
	u32 data;

	data = rdl(mp, PHY_ADDR);
	data &= ~(0x1f << addr_shift);
	data |= (phy_addr & 0x1f) << addr_shift;
	wrl(mp, PHY_ADDR, data);
}

static int phy_addr_get(struct mv643xx_eth_private *mp)
{
	unsigned int data;

	data = rdl(mp, PHY_ADDR);

	return (data >> (5 * mp->port_num)) & 0x1f;
}

static void set_params(struct mv643xx_eth_private *mp,
		       struct mv643xx_eth_platform_data *pd)
{
	struct net_device *dev = mp->dev;
	unsigned int tx_ring_size;

	if (is_valid_ether_addr(pd->mac_addr))
		memcpy(dev->dev_addr, pd->mac_addr, ETH_ALEN);
	else
		uc_addr_get(mp, dev->dev_addr);

	mp->rx_ring_size = DEFAULT_RX_QUEUE_SIZE;
	if (pd->rx_queue_size)
		mp->rx_ring_size = pd->rx_queue_size;
	mp->rx_desc_sram_addr = pd->rx_sram_addr;
	mp->rx_desc_sram_size = pd->rx_sram_size;

	mp->rxq_count = pd->rx_queue_count ? : 1;

	tx_ring_size = DEFAULT_TX_QUEUE_SIZE;
	if (pd->tx_queue_size)
		tx_ring_size = pd->tx_queue_size;

	mp->tx_ring_size = clamp_t(unsigned int, tx_ring_size,
				   MV643XX_MAX_SKB_DESCS * 2, 4096);
	if (mp->tx_ring_size != tx_ring_size)
		netdev_warn(dev, "TX queue size set to %u (requested %u)\n",
			    mp->tx_ring_size, tx_ring_size);

	mp->tx_desc_sram_addr = pd->tx_sram_addr;
	mp->tx_desc_sram_size = pd->tx_sram_size;

	mp->txq_count = pd->tx_queue_count ? : 1;
	mp->txq_count += NAPI_TX_OFFSET;
}

static struct phy_device *phy_scan(struct mv643xx_eth_private *mp,
				   int phy_addr)
{
	struct phy_device *phydev;
	int start;
	int num;
	int i;
	char phy_id[MII_BUS_ID_SIZE + 3];

	if (phy_addr == MV643XX_ETH_PHY_ADDR_DEFAULT) {
		start = phy_addr_get(mp) & 0x1f;
		num = 32;
	} else {
		start = phy_addr & 0x1f;
		num = 1;
	}

	/* Attempt to connect to the PHY using orion-mdio */
	phydev = ERR_PTR(-ENODEV);
	for (i = 0; i < num; i++) {
		int addr = (start + i) & 0x1f;

		snprintf(phy_id, sizeof(phy_id), PHY_ID_FMT,
				"orion-mdio-mii", addr);

		phydev = phy_connect(mp->dev, phy_id, mv643xx_eth_adjust_link,
				PHY_INTERFACE_MODE_GMII);
		if (!IS_ERR(phydev)) {
			phy_addr_set(mp, addr);
			break;
		}
	}

	return phydev;
}

static void phy_init(struct mv643xx_eth_private *mp, int speed, int duplex)
{
	struct phy_device *phy = mp->phy;

	if (speed == 0) {
		phy->autoneg = AUTONEG_ENABLE;
		phy->speed = 0;
		phy->duplex = 0;
		phy->advertising = phy->supported | ADVERTISED_Autoneg;
	} else {
		phy->autoneg = AUTONEG_DISABLE;
		phy->advertising = 0;
		phy->speed = speed;
		phy->duplex = duplex;
	}
	phy_start_aneg(phy);
}

static int mii_bus_read(struct net_device *dev, int mii_id, int regnum)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	if (!mp->mii_bus)
		return 0xffff;
	return mp->mii_bus->read(mp->mii_bus, mii_id, regnum);
}

static void mii_bus_write(struct net_device *dev, int mii_id, int regnum,
			 int value)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	if (!mp->mii_bus)
		return ;
	mp->mii_bus->write(mp->mii_bus, mii_id, regnum, value);
}

static int mii_bus_init(struct net_device *dev,
			struct platform_device *pdev,
			struct mv643xx_eth_platform_data *pd)
{
	struct mv643xx_eth_private *mp = netdev_priv(dev);
	extern void fbxgw_common_switch_init(struct net_device *dev,
			     typeof(mii_bus_read), typeof(mii_bus_write));



	mp->mii_bus = mdio_find_bus("f1072004.mdio-bu");
	if (!mp->mii_bus) {
		dev_err(&pdev->dev, "unable to find mdio bus f1072004.mdio-bu");
		return -ENODEV;
	}

#ifdef CONFIG_FBXGW_COMMON
	fbxgw_common_switch_init(dev, mii_bus_read, mii_bus_write);
#endif
	return 0;
}

static void init_pscr(struct mv643xx_eth_private *mp, int speed, int duplex)
{
	u32 pscr;

	pscr = rdlp(mp, PORT_SERIAL_CONTROL);
	if (pscr & SERIAL_PORT_ENABLE) {
		pscr &= ~SERIAL_PORT_ENABLE;
		wrlp(mp, PORT_SERIAL_CONTROL, pscr);
	}

	pscr = MAX_RX_PACKET_9700BYTE | SERIAL_PORT_CONTROL_RESERVED;
	if (mp->phy == NULL) {
		pscr |= DISABLE_AUTO_NEG_SPEED_GMII;
		if (speed == SPEED_1000)
			pscr |= SET_GMII_SPEED_TO_1000;
		else if (speed == SPEED_100)
			pscr |= SET_MII_SPEED_TO_100;

		pscr |= DISABLE_AUTO_NEG_FOR_FLOW_CTRL;

		pscr |= DISABLE_AUTO_NEG_FOR_DUPLEX;
		if (duplex == DUPLEX_FULL)
			pscr |= SET_FULL_DUPLEX_MODE;
	}

	wrlp(mp, PORT_SERIAL_CONTROL, pscr);
}

static const struct net_device_ops mv643xx_eth_netdev_ops = {
	.ndo_open		= mv643xx_eth_open,
	.ndo_stop		= mv643xx_eth_stop,
	.ndo_start_xmit		= mv643xx_eth_xmit,
	.ndo_set_rx_mode	= mv643xx_eth_set_rx_mode,
	.ndo_set_mac_address	= mv643xx_eth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= mv643xx_eth_ioctl,
	.ndo_change_mtu		= mv643xx_eth_change_mtu,
	.ndo_set_features	= mv643xx_eth_set_features,
	.ndo_tx_timeout		= mv643xx_eth_tx_timeout,
	.ndo_get_stats		= mv643xx_eth_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= mv643xx_eth_netpoll,
#endif
};

static int mv643xx_eth_probe(struct platform_device *pdev)
{
	struct mv643xx_eth_platform_data *pd;
	struct mv643xx_eth_private *mp;
	struct net_device *dev;
	struct resource *res;
	int err;

	pd = dev_get_platdata(&pdev->dev);
	if (pd == NULL) {
		dev_err(&pdev->dev, "no mv643xx_eth_platform_data\n");
		return -ENODEV;
	}

	if (pd->shared == NULL) {
		dev_err(&pdev->dev, "no mv643xx_eth_platform_data->shared\n");
		return -ENODEV;
	}

	dev = alloc_etherdev_mqs(sizeof(struct mv643xx_eth_private),
				 pd->tx_queue_count ? : 1,
				 pd->rx_queue_count ? : 1);
	if (!dev)
		return -ENOMEM;

	mp = netdev_priv(dev);
	platform_set_drvdata(pdev, mp);

	mp->shared = platform_get_drvdata(pd->shared);
	mp->base = mp->shared->base + 0x0400 + (pd->port_number << 10);
	mp->port_num = pd->port_number;

	mp->dev = dev;

	/* Kirkwood resets some registers on gated clocks. Especially
	 * CLK125_BYPASS_EN must be cleared but is not available on
	 * all other SoCs/System Controllers using this driver.
	 */
	if (of_device_is_compatible(pdev->dev.of_node,
				    "marvell,kirkwood-eth-port"))
		wrlp(mp, PORT_SERIAL_CONTROL1,
		     rdlp(mp, PORT_SERIAL_CONTROL1) & ~CLK125_BYPASS_EN);

	/*
	 * Start with a default rate, and if there is a clock, allow
	 * it to override the default.
	 */
	mp->t_clk = 133000000;
	mp->clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(mp->clk)) {
		clk_prepare_enable(mp->clk);
		mp->t_clk = clk_get_rate(mp->clk);
	} else if (!IS_ERR(mp->shared->clk)) {
		mp->t_clk = clk_get_rate(mp->shared->clk);
	}

	set_params(mp, pd);
	netif_set_real_num_tx_queues(dev, mp->txq_count - NAPI_TX_OFFSET);
	netif_set_real_num_rx_queues(dev, mp->rxq_count);

	err = 0;
	if (pd->phy_node) {
		mp->phy = of_phy_connect(mp->dev, pd->phy_node,
					 mv643xx_eth_adjust_link, 0,
					 PHY_INTERFACE_MODE_GMII);
		if (!mp->phy)
			err = -ENODEV;
		else
			phy_addr_set(mp, mp->phy->addr);
	} else if (pd->phy_addr != MV643XX_ETH_PHY_NONE) {
		mp->phy = phy_scan(mp, pd->phy_addr);

		if (IS_ERR(mp->phy))
			err = PTR_ERR(mp->phy);
		else
			phy_init(mp, pd->speed, pd->duplex);
	} else {
		mii_bus_init(dev, pdev, pd);
	}
	if (err == -ENODEV) {
		err = -EPROBE_DEFER;
		goto out;
	}
	if (err)
		goto out;

	dev->ethtool_ops = &mv643xx_eth_ethtool_ops;

	init_pscr(mp, pd->speed, pd->duplex);


	mib_counters_clear(mp);

	setup_timer(&mp->mib_counters_timer, mib_counters_timer_wrapper,
		    (unsigned long)mp);
	mp->mib_counters_timer.expires = jiffies + 30 * HZ;

	spin_lock_init(&mp->mib_counters_lock);

	INIT_WORK(&mp->tx_timeout_task, tx_timeout_task);

	netif_napi_add(dev, &mp->napi, mv643xx_eth_poll, NAPI_POLL_WEIGHT);


	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	BUG_ON(!res);
	dev->irq = res->start;

	dev->netdev_ops = &mv643xx_eth_netdev_ops;

	dev->watchdog_timeo = 2 * HZ;
	dev->base_addr = 0;

	dev->features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_TSO;
	dev->vlan_features = dev->features;

	dev->features |= NETIF_F_RXCSUM;
	dev->hw_features = dev->features;

	dev->priv_flags |= IFF_UNICAST_FLT;
	dev->gso_max_segs = MV643XX_MAX_TSO_SEGS;

	SET_NETDEV_DEV(dev, &pdev->dev);

	if (mp->shared->win_protect)
		wrl(mp, WINDOW_PROTECT(mp->port_num), mp->shared->win_protect);

	netif_carrier_off(dev);

	wrlp(mp, SDMA_CONFIG, PORT_SDMA_CONFIG_DEFAULT_VALUE);

	set_rx_coal(mp, 250);
	set_tx_coal(mp, 0);

	err = register_netdev(dev);
	if (err)
		goto out;

	netdev_notice(dev, "port %d with MAC address %pM\n",
		      mp->port_num, dev->dev_addr);

	if (mp->tx_desc_sram_size > 0)
		netdev_notice(dev, "configured with sram\n");

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	mp->ff_notifier.notifier_call = ff_device_event;
	register_netdevice_notifier(&mp->ff_notifier);
#endif

	return 0;

out:
	if (!IS_ERR(mp->clk))
		clk_disable_unprepare(mp->clk);
	free_netdev(dev);

	return err;
}

static int mv643xx_eth_remove(struct platform_device *pdev)
{
	struct mv643xx_eth_private *mp = platform_get_drvdata(pdev);

#ifdef CONFIG_MV643XX_ETH_FBX_FF
	unregister_netdevice_notifier(&mp->ff_notifier);
#endif

	unregister_netdev(mp->dev);
	if (mp->phy != NULL)
		phy_disconnect(mp->phy);
	cancel_work_sync(&mp->tx_timeout_task);

	if (!IS_ERR(mp->clk))
		clk_disable_unprepare(mp->clk);

	free_netdev(mp->dev);

	return 0;
}

static void mv643xx_eth_shutdown(struct platform_device *pdev)
{
	struct mv643xx_eth_private *mp = platform_get_drvdata(pdev);

	/* Mask all interrupts on ethernet port */
	wrlp(mp, INT_MASK, 0);
	rdlp(mp, INT_MASK);

	if (netif_running(mp->dev))
		port_reset(mp);
}

static struct platform_driver mv643xx_eth_driver = {
	.probe		= mv643xx_eth_probe,
	.remove		= mv643xx_eth_remove,
	.shutdown	= mv643xx_eth_shutdown,
	.driver = {
		.name	= MV643XX_ETH_NAME,
	},
};

static int __init mv643xx_eth_init_module(void)
{
	int rc;

	rc = platform_driver_register(&mv643xx_eth_shared_driver);
	if (!rc) {
		rc = platform_driver_register(&mv643xx_eth_driver);
		if (rc)
			platform_driver_unregister(&mv643xx_eth_shared_driver);
	}

	return rc;
}
module_init(mv643xx_eth_init_module);

static void __exit mv643xx_eth_cleanup_module(void)
{
	platform_driver_unregister(&mv643xx_eth_driver);
	platform_driver_unregister(&mv643xx_eth_shared_driver);
}
module_exit(mv643xx_eth_cleanup_module);

MODULE_AUTHOR("Rabeeh Khoury, Assaf Hoffman, Matthew Dharm, "
	      "Manish Lachwani, Dale Farnsworth and Lennert Buytenhek");
MODULE_DESCRIPTION("Ethernet driver for Marvell MV643XX");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" MV643XX_ETH_SHARED_NAME);
MODULE_ALIAS("platform:" MV643XX_ETH_NAME);
