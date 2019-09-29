#ifndef IP_FFN_H_
#define IP_FFN_H_

#include <linux/types.h>
#include <linux/net.h>
#include <net/route.h>
#include <net/netfilter/nf_conntrack.h>

struct ffn_data {
	u32 new_sip;
	u32 new_dip;
	u16 new_sport;
	u16 new_dport;
	u8 new_tos;
	u8 force_skb_prio : 1;
	u8 alter : 1;
	u8 tos_change : 1;
	__sum16 ip_adjustment;
	__sum16 l4_adjustment;
	unsigned int new_skb_prio;
	u32 new_mark;
	struct dst_entry *dst;
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;

	void (*priv_destructor)(void *);
	u32 ffn_priv_area[8];
};

struct ffn_lookup_entry {
	int added_when;
	u32 sip;
	u32 dip;
	u16 sport;
	u16 dport;
	u8 protocol;
#ifdef CONFIG_IP_FFN_PROCFS
	uint64_t forwarded_bytes;
	uint32_t forwarded_packets;
#endif
	struct list_head next;
	struct ffn_data manip;
	struct list_head all_next;
};

extern spinlock_t ffn_lock;
struct ffn_lookup_entry *__ffn_get(u32 sip, u32 dip,
				   u16 sport, u16 dport,
				   int is_tcp);

#endif /* ! IP_FFN_H_*/
