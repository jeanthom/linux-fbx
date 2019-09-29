/*
 * IP fast forwarding and NAT
 *
 * Very restrictive code, that only cope non fragmented UDP and TCP
 * packets, that are routed and NATed with no other modification.
 *
 * Provide a fast path for established conntrack entries so that
 * packets go out ASAP.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/jhash.h>
#include <linux/proc_fs.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>

#include <net/ip_ffn.h>

#define FFN_CACHE_SIZE		256
#define MAX_FFN_ENTRY		2048

DEFINE_SPINLOCK(ffn_lock);
static struct list_head ffn_cache[FFN_CACHE_SIZE];
static struct list_head ffn_all;
static unsigned int ffn_entry_count;

/*
 * hash on five parameter
 */
static inline unsigned int ffn_hash(u32 sip, u32 dip, u16 sport, u16 dport,
				    int is_tcp)
{
	return jhash_3words(sip, is_tcp ? dip : ~dip, sport | dport << 16, 0);
}

/*
 * attempt to find entry with given value in cache
 */
static struct ffn_lookup_entry *__ffn_find(u32 sip, u32 dip,
					   u16 sport, u16 dport,
					   u8 protocol,
					   unsigned int hash)
{
	struct ffn_lookup_entry *tmp;

	list_for_each_entry(tmp, &ffn_cache[hash % FFN_CACHE_SIZE], next) {

		/* compare entry */
		if (tmp->sip == sip && tmp->dip == dip &&
		    tmp->sport == sport && tmp->dport == dport &&
		    tmp->protocol == protocol)
			return tmp;
	}
	return NULL;
}

struct ffn_lookup_entry *__ffn_get(u32 sip, u32 dip,
				   u16 sport, u16 dport,
				   int is_tcp)
{
	unsigned int hash;
	u8 protocol;

	/* lookup entry in cache */
	protocol = (is_tcp) ? IPPROTO_TCP : IPPROTO_UDP;
	hash = ffn_hash(sip, dip, sport, dport, is_tcp);
	return __ffn_find(sip, dip, sport, dport, protocol, hash);
}

static void __ffn_remove_entry(struct ffn_lookup_entry *e)
{
	if (e->manip.priv_destructor)
		e->manip.priv_destructor((void *)e->manip.ffn_priv_area);
	list_del(&e->next);
	list_del(&e->all_next);
	ffn_entry_count--;
	dst_release(e->manip.dst);
	kfree(e);
}

static int __ffn_add_entry(struct ffn_lookup_entry *e,
			   u8 proto, unsigned int hash)
{
	/* make sure it's not present */
	if (__ffn_find(e->sip, e->dip, e->sport, e->dport, proto, hash))
		return 1;

	if (ffn_entry_count >= MAX_FFN_ENTRY)
		return 1;

	/* add new entry */
	list_add_tail(&e->next, &ffn_cache[hash % FFN_CACHE_SIZE]);
	list_add_tail(&e->all_next, &ffn_all);
	ffn_entry_count++;
	return 0;
}

/*
 *
 */
static inline __sum16 checksum_adjust(u32 osip,
				      u32 nsip,
				      u32 odip,
				      u32 ndip,
				      u16 osport,
				      u16 nsport,
				      u16 odport,
				      u16 ndport)
{
	const u32 old[] = { osip, odip, osport, odport };
	const u32 new[] = { nsip, ndip, nsport, ndport };
	__wsum osum, nsum;

	osum = csum_partial(old, sizeof (old), 0);
	nsum = csum_partial(new, sizeof (new), 0);

	return ~csum_fold(csum_sub(nsum, osum));
}

/*
 *
 */
static inline __sum16 checksum_adjust_ip(u32 osip,
					 u32 nsip,
					 u32 odip,
					 u32 ndip)
{
	const u32 old[] = { osip, odip };
	const u32 new[] = { nsip, ndip };
	__wsum osum, nsum;

	osum = csum_partial(old, sizeof (old), 0);
	nsum = csum_partial(new, sizeof (new), 0);

	/* -1 for TTL decrease */
	return ~csum_fold(csum_sub(csum_sub(nsum, osum), 1));
}

/*
 * two hooks into netfilter code
 */
extern int external_tcpv4_packet(struct nf_conn *ct,
				 const struct sk_buff *skb,
				 unsigned int dataoff,
				 enum ip_conntrack_info ctinfo);

extern int external_udpv4_packet(struct nf_conn *ct,
				 const struct sk_buff *skb,
				 unsigned int dataoff,
				 enum ip_conntrack_info ctinfo);

extern int ip_local_deliver_finish(struct sock *sk, struct sk_buff *skb);

/*
 * check if packet is in ffn cache, or mark it if it can be added
 * later
 */
int ip_ffn_process(struct sk_buff *skb)
{
	struct ffn_lookup_entry *e;
	struct nf_conntrack *nfct;
	struct iphdr *iph;
	struct tcphdr *tcph = NULL;
	struct udphdr *udph = NULL;
	bool remove_me;
	u16 tcheck;
	u8 proto;
	int res, added_when;

	if (!net_eq(dev_net(skb->dev), &init_net))
		goto not_ffnable;

	iph = ip_hdr(skb);

	/* refuse fragmented IP packet, or packets with IP options */
	if (iph->ihl > 5 || (iph->frag_off & htons(IP_MF | IP_OFFSET)))
		goto not_ffnable;

	/* check encapsulated protocol is udp or tcp */
	if (iph->protocol != IPPROTO_TCP && iph->protocol != IPPROTO_UDP)
		goto not_ffnable;

	if (iph->ttl <= 1)
		goto not_ffnable;

	proto = iph->protocol;
	if (proto == IPPROTO_TCP) {
		if (skb_headlen(skb) < sizeof (*iph) + sizeof (struct tcphdr))
			goto not_ffnable;

		tcph = (struct tcphdr *)((unsigned char *)iph + sizeof (*iph));

		if (tcph->doff * 4 < sizeof (struct tcphdr))
			goto not_ffnable;

		if (skb_headlen(skb) < sizeof (*iph) + tcph->doff * 4)
			goto not_ffnable;

		spin_lock_bh(&ffn_lock);
		e = __ffn_get(iph->saddr, iph->daddr, tcph->source,
			      tcph->dest, 1);
	} else {
		if (skb_headlen(skb) < sizeof (*iph) + sizeof (struct udphdr))
			goto not_ffnable;

		udph = (struct udphdr *)((unsigned char *)iph + sizeof (*iph));
		spin_lock_bh(&ffn_lock);
		e = __ffn_get(iph->saddr, iph->daddr, udph->source,
			      udph->dest, 0);
	}

	if (!e) {
		spin_unlock_bh(&ffn_lock);
		goto ffnable;
	}

	if (e->manip.dst->obsolete > 0) {
		__ffn_remove_entry(e);
		spin_unlock_bh(&ffn_lock);
		goto ffnable;
	}

	nfct = &e->manip.ct->ct_general;
	nf_conntrack_get(nfct);

	remove_me = false;
	if (proto == IPPROTO_TCP) {
		/* do sequence number checking and update
		 * conntrack info */
		res = external_tcpv4_packet(e->manip.ct, skb, sizeof (*iph),
					    e->manip.ctinfo);
		if (e->manip.ct->proto.tcp.state != TCP_ESTABLISHED)
			remove_me = true;
		tcheck = tcph->check;

	} else {
		res = external_udpv4_packet(e->manip.ct, skb, sizeof (*iph),
					    e->manip.ctinfo);
		tcheck = udph->check;
	}

	if (unlikely(res != NF_ACCEPT)) {
		/* packet rejected by conntrack, unless asked to drop,
		 * send it back into kernel */
		if (remove_me)
			__ffn_remove_entry(e);

		spin_unlock_bh(&ffn_lock);
		nf_conntrack_put(nfct);

		if (res == NF_DROP) {
			dev_kfree_skb(skb);
			return 0;
		}

		goto ffnable;
	}

	if (!e->manip.alter)
		goto fix_ip_hdr;

	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		/* fix ports & transport protocol checksum */
		if (proto == IPPROTO_TCP) {
			tcph->source = e->manip.new_sport;
			tcph->dest = e->manip.new_dport;
			tcph->check = csum16_sub(tcph->check,
						 e->manip.l4_adjustment);
		} else {
			udph->source = e->manip.new_sport;
			udph->dest = e->manip.new_dport;
			if (udph->check) {
				u16 tcheck;

				tcheck = csum16_sub(udph->check,
						    e->manip.l4_adjustment);
				udph->check = tcheck ? tcheck : 0xffff;
			}
		}
	} else {
		unsigned int len;

		/*
		 * assume tcph->check only covers ip pseudo header, so
		 * don't update checksum wrt port change
		 *
		 * we might check skb->csum_offset to confirm that
		 * this is a valid assertion
		 */
		if (proto == IPPROTO_TCP) {
			len = skb->len - ((void *)tcph - (void *)iph);
			tcheck = ~csum_tcpudp_magic(e->manip.new_sip,
						    e->manip.new_dip,
						    len, IPPROTO_TCP, 0);
			tcph->check = tcheck;
			tcph->source = e->manip.new_sport;
			tcph->dest = e->manip.new_dport;
		} else {
			len = skb->len - ((void *)udph - (void *)iph);
			if (udph->check) {
				tcheck = ~csum_tcpudp_magic(e->manip.new_sip,
							    e->manip.new_dip,
							    len,
							    IPPROTO_UDP, 0);
				udph->check = tcheck ? tcheck : 0xffff;
			}
			udph->source = e->manip.new_sport;
			udph->dest = e->manip.new_dport;
		}
	}

	/* update IP header field */
	iph->saddr = e->manip.new_sip;
	iph->daddr = e->manip.new_dip;

fix_ip_hdr:
	iph->ttl--;

	if (e->manip.tos_change) {
		iph->tos = e->manip.new_tos;
		iph->check = 0;
		iph->check = ip_fast_csum((u8 *)iph, 5);
	} else {
		iph->check = csum16_sub(iph->check,
					e->manip.ip_adjustment);
	}

	/* forward skb */
	if (e->manip.force_skb_prio)
		skb->priority = e->manip.new_skb_prio;
	else
		skb->priority = rt_tos2priority(iph->tos);

	skb->mark = e->manip.new_mark;

#ifdef CONFIG_IP_FFN_PROCFS
	e->forwarded_packets++;
	e->forwarded_bytes += skb->len;
#endif

	skb_dst_set(skb, dst_clone(e->manip.dst));

	if (nfct == skb->nfct) {
		/*
		 * skbs to/from localhost will have the conntrack
		 * already set, don't leak references here.
		 */
		nf_conntrack_put(nfct);
	} else {
		if (unlikely(skb->nfct != NULL)) {
			/*
			 * conntrack is not NULL here and it is not
			 * the same as the one we have in the
			 * ffn_entry, this shoud not happen, warn once
			 * and switch to slow path.
			 */
			WARN_ONCE(skb->nfct != NULL,
				  "weird skb->nfct %p, NULL was expected\n",
				  skb->nfct);
			printk_once(KERN_WARNING "ffn entry:\n"
				    " added_when: %i\n"
				    " sip: %pI4 -> %pI4\n"
				    " dip: %pI4 -> %pI4\n"
				    " sport: %u -> %u\n"
				    " dport: %u -> %u\n",
				    e->added_when,
				    &e->sip, &e->manip.new_sip,
				    &e->dip, &e->manip.new_dip,
				    htons(e->sport), htons(e->manip.new_sport),
				    htons(e->dport), htons(e->manip.new_dport));
			goto not_ffnable;
		}
		skb->nfct = nfct;
		skb->nfctinfo = e->manip.ctinfo;
	}

	added_when = e->added_when;
	if (remove_me)
		__ffn_remove_entry(e);
	spin_unlock_bh(&ffn_lock);

	skb->ffn_state = FFN_STATE_FAST_FORWARDED;

	if (added_when == IP_FFN_FINISH_OUT)
		dst_output(skb);
	else
		ip_local_deliver_finish(skb->sk, skb);

	return 0;

ffnable:
	skb->ffn_state = FFN_STATE_FORWARDABLE;
	skb->ffn_orig_tos = iph->tos;
	return 1;

not_ffnable:
	skb->ffn_state = FFN_STATE_INCOMPATIBLE;
	return 1;
}

/*
 * check if skb is candidate for ffn, and if so add it to ffn cache
 *
 * called after post routing
 */
void ip_ffn_add(struct sk_buff *skb, int when)
{
	struct nf_conn *ct;
	struct nf_conntrack_tuple *tuple, *rtuple;
	enum ip_conntrack_info ctinfo;
	struct ffn_lookup_entry *e;
	struct iphdr *iph;
	unsigned int hash;
	int dir;
	u8 proto;

	if (!net_eq(dev_net(skb->dev), &init_net))
		return;

	if (ffn_entry_count >= MAX_FFN_ENTRY)
		return;

	iph = ip_hdr(skb);

	if (!skb->nfct)
		return;

	if (skb_dst(skb)->output != ip_output && when == IP_FFN_FINISH_OUT)
		return;

	if (skb->nfct == &nf_ct_untracked_get()->ct_general)
		return;

	ct = nf_ct_get(skb, &ctinfo);
	if ((ctinfo != IP_CT_ESTABLISHED) &&
	    (ctinfo != IP_CT_ESTABLISHED + IP_CT_IS_REPLY)) {
		return;
	}

	if (nfct_help(ct))
		return;

	dir = (ctinfo == IP_CT_ESTABLISHED) ?
		IP_CT_DIR_ORIGINAL : IP_CT_DIR_REPLY;
	tuple = &ct->tuplehash[dir].tuple;

	if (tuple->dst.protonum != IPPROTO_TCP &&
	    tuple->dst.protonum != IPPROTO_UDP)
		return;

	if (tuple->dst.protonum == IPPROTO_TCP &&
	    ct->proto.tcp.state != TCP_CONNTRACK_ESTABLISHED)
		return;

	rtuple = &ct->tuplehash[1 - dir].tuple;

	e = kmalloc(sizeof (*e), GFP_ATOMIC);
	if (!e)
		return;

	e->added_when = when;
	e->sip = tuple->src.u3.ip;
	e->dip = tuple->dst.u3.ip;
	e->sport = tuple->src.u.all;
	e->dport = tuple->dst.u.all;
	e->protocol = tuple->dst.protonum;

#ifdef CONFIG_IP_FFN_PROCFS
	e->forwarded_packets = 0;
	e->forwarded_bytes = 0;
#endif

	e->manip.new_sip = rtuple->dst.u3.ip;
	e->manip.new_dip = rtuple->src.u3.ip;
	e->manip.new_sport = rtuple->dst.u.all;
	e->manip.new_dport = rtuple->src.u.all;

	if (e->manip.new_sip == e->sip &&
	    e->manip.new_dip == e->dip &&
	    e->manip.new_sport == e->sport &&
	    e->manip.new_dport == e->dport)
		e->manip.alter = 0;
	else
		e->manip.alter = 1;

	if (e->manip.alter) {
		/* compute checksum adjustement */
		e->manip.l4_adjustment = checksum_adjust(e->sip,
							 e->manip.new_sip,
							 e->dip,
							 e->manip.new_dip,
							 e->sport,
							 e->manip.new_sport,
							 e->dport,
							 e->manip.new_dport);

		e->manip.ip_adjustment = checksum_adjust_ip(e->sip,
							    e->manip.new_sip,
							    e->dip,
							    e->manip.new_dip);
	}

	if (skb->ffn_orig_tos != iph->tos) {
		e->manip.tos_change = 1;
		e->manip.new_tos = iph->tos;
	} else
		e->manip.tos_change = 0;

	if (skb->priority != rt_tos2priority(iph->tos)) {
		e->manip.force_skb_prio = 1;
		e->manip.new_skb_prio = skb->priority;
	} else
		e->manip.force_skb_prio = 0;

	e->manip.new_mark = skb->mark;
	e->manip.priv_destructor = NULL;
	e->manip.dst = skb_dst(skb);
	dst_hold(e->manip.dst);
	e->manip.ct = ct;
	e->manip.ctinfo = ctinfo;

	hash = ffn_hash(e->sip, e->dip, e->sport, e->dport,
			e->protocol == IPPROTO_TCP);
	proto = (e->protocol == IPPROTO_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

	spin_lock_bh(&ffn_lock);
	if (__ffn_add_entry(e, proto, hash)) {
		spin_unlock_bh(&ffn_lock);
		dst_release(e->manip.dst);
		kfree(e);
		return;
	}
	spin_unlock_bh(&ffn_lock);
}

/*
 * netfilter callback when conntrack is about to be destroyed
 */
void ip_ffn_ct_destroy(struct nf_conn *ct)
{
	struct nf_conntrack_tuple *tuple;
	struct ffn_lookup_entry *e;
	int dir;

	/* locate all entry that use this conntrack */
	for (dir = 0; dir < 2; dir++) {
		tuple = &ct->tuplehash[dir].tuple;

		if (tuple->dst.protonum != IPPROTO_TCP &&
		    tuple->dst.protonum != IPPROTO_UDP)
			return;

		spin_lock_bh(&ffn_lock);
		e = __ffn_get(tuple->src.u3.ip, tuple->dst.u3.ip,
			      tuple->src.u.all, tuple->dst.u.all,
			      tuple->dst.protonum == IPPROTO_TCP);
		if (e)
			__ffn_remove_entry(e);
		spin_unlock_bh(&ffn_lock);
	}
}

/*
 * initialize ffn cache data
 */
static void __ip_ffn_init_cache(void)
{
	int i;

	for (i = 0; i < FFN_CACHE_SIZE; i++)
		INIT_LIST_HEAD(&ffn_cache[i]);
	INIT_LIST_HEAD(&ffn_all);
	ffn_entry_count = 0;
}

/*
 * flush all ffn cache
 */
void ip_ffn_flush_all(void)
{
	struct ffn_lookup_entry *e, *tmp;

	spin_lock_bh(&ffn_lock);
	list_for_each_entry_safe(e, tmp, &ffn_all, all_next)
		__ffn_remove_entry(e);
	__ip_ffn_init_cache();
	spin_unlock_bh(&ffn_lock);
}

#ifdef CONFIG_IP_FFN_PROCFS
struct proc_dir_entry *proc_net_ip_ffn;

static int ip_ffn_entries_show(struct seq_file *m, void *v)
{
	int i;

	spin_lock_bh(&ffn_lock);

	for (i = 0; i < FFN_CACHE_SIZE; ++i) {
		struct ffn_lookup_entry *e;

		if (list_empty(&ffn_cache[i]))
			continue;

		seq_printf(m, "Bucket %i:\n", i);
		list_for_each_entry (e, &ffn_cache[i], next) {
			seq_printf(m, " Protocol: ");
			switch (e->protocol) {
			case IPPROTO_TCP:
				seq_printf(m, "TCPv4\n");
				break;
			case IPPROTO_UDP:
				seq_printf(m, "UDPv4\n");
				break;
			default:
				seq_printf(m, "ipproto_%i\n", e->protocol);
				break;
			}
			seq_printf(m, " Original flow: %pI4:%u -> %pI4:%u\n",
				   &e->sip,
				   ntohs(e->sport),
				   &e->dip,
				   ntohs(e->dport));

			if (e->sip != e->manip.new_sip ||
			    e->dip != e->manip.new_dip ||
			    e->sport != e->manip.new_sport ||
			    e->dport != e->manip.new_dport) {
				seq_printf(m,
					   " Modified flow: %pI4:%u -> "
					   "%pI4:%u\n",
					   &e->manip.new_sip,
					   ntohs(e->manip.new_sport),
					   &e->manip.new_dip,
					   ntohs(e->manip.new_dport));
			}

			seq_printf(m, "  Forwarded packets: %u\n",
				   e->forwarded_packets);
			seq_printf(m, "  Forwarded bytes: %llu\n",
				   e->forwarded_bytes);
			seq_printf(m, "\n");
		}
	}

	spin_unlock_bh(&ffn_lock);
	return 0;
}

static int ip_ffn_entries_open(struct inode *inode, struct file *file)
{
	return single_open(file, ip_ffn_entries_show, NULL);
}

static const struct file_operations ip_ffn_entries_fops = {
	.owner = THIS_MODULE,
	.open	= ip_ffn_entries_open,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek,
};


static int __init __ip_ffn_init_procfs(void)
{
	proc_net_ip_ffn = proc_net_mkdir(&init_net, "ip_ffn",
					 init_net.proc_net);
	if (!proc_net_ip_ffn) {
		printk(KERN_ERR "proc_mkdir() has failed for 'net/ip_ffn'.\n");
		return -1;
	}

	if (proc_create("entries", 0400, proc_net_ip_ffn,
			&ip_ffn_entries_fops) == NULL) {
		printk(KERN_ERR "proc_create() has failed for "
		       "'net/ip_ffn/entries'.\n");
		return -1;
	}
	return 0;
}
#endif

/*
 * initialize ffn
 */
void __init ip_ffn_init(void)
{
	printk("IP Fast Forward and NAT enabled\n");
	__ip_ffn_init_cache();

#ifdef CONFIG_IP_FFN_PROCFS
	if (__ip_ffn_init_procfs() < 0)
		printk(KERN_WARNING "IP FFN: unable to create proc entries.\n");
#endif
}
