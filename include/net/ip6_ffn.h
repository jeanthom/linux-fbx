#ifndef IP6_FFN_H_
#define IP6_FFN_H_

#include <linux/types.h>
#include <linux/net.h>
#include <net/route.h>
#include <net/netfilter/nf_conntrack.h>

struct ffn6_data {
	u32 new_sip[4];
	u32 new_dip[4];

	u16 new_sport;
	u16 new_dport;
	__sum16 adjustment;
	u8 new_tos;
	u32 new_skb_prio;
	u32 new_mark;

	u32 force_skb_prio : 1;
	u32 alter : 1;
	u32 tos_change : 1;
	struct dst_entry *dst;
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;

	void (*priv_destructor)(void *);
	u32 ffn_priv_area[8];
};

struct ffn6_lookup_entry {
	u32 sip[4];
	u32 dip[4];
	u16 sport;
	u16 dport;
	u8 protocol;
	u8 added_when;
#ifdef CONFIG_IPV6_FFN_PROCFS
	uint64_t forwarded_bytes;
	uint32_t forwarded_packets;
#endif
	struct list_head next;
	struct ffn6_data manip;
	struct list_head all_next;
};

extern spinlock_t ffn6_lock;
struct ffn6_lookup_entry *__ffn6_get(const u32 *sip,
				     const u32 *dip,
				     u16 sport, u16 dport,
				     int is_tcp);

#endif /* ! IP6_FFN_H_*/
