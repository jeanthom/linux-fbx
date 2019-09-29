/*
 * IPv6 fast forwarding and NAT
 *
 * Very restrictive code, that only cope non fragmented UDP and TCP
 * packets, that are routed and NATed with no other modification.
 *
 * Provide a fast path for established conntrack entries so that
 * packets go out ASAP.
 */

#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/jhash.h>
#include <linux/proc_fs.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>

#include <net/ip6_ffn.h>
#include <net/dsfield.h>

#define FFN6_CACHE_SIZE		256
#define MAX_FFN6_ENTRY		2048

DEFINE_SPINLOCK(ffn6_lock);
static struct list_head ffn6_cache[FFN6_CACHE_SIZE];
static struct list_head ffn6_all;
static unsigned int ffn6_entry_count;

static void __ffn6_remove_entry(struct ffn6_lookup_entry *e)
{
	if (e->manip.priv_destructor)
		e->manip.priv_destructor((void *)e->manip.ffn_priv_area);
	list_del(&e->next);
	list_del(&e->all_next);
	ffn6_entry_count--;
	dst_release(e->manip.dst);
	kfree(e);
}

/*
 * hash on five parameter
 */
static inline unsigned int ffn6_hash(const u32 *sip, const u32 *dip,
				     u16 sport, u16 dport,
				     int is_tcp)
{
	return jhash_3words(sip[3], is_tcp ? dip[3] : ~dip[3],
			    sport | dport << 16, 0);
}

/*
 * attempt to find entry with given value in cache
 */
static struct ffn6_lookup_entry *__ffn6_find(const u32 *sip, const u32 *dip,
					     u16 sport, u16 dport,
					     u8 protocol,
					     unsigned int hash)
{
	struct ffn6_lookup_entry *tmp;

	list_for_each_entry(tmp, &ffn6_cache[hash % FFN6_CACHE_SIZE], next) {

		/* compare entry */
		if (!memcmp(tmp->sip, sip, 16) &&
		    !memcmp(tmp->dip, dip, 16) &&
		    tmp->sport == sport && tmp->dport == dport &&
		    tmp->protocol == protocol)
			return tmp;
	}
	return NULL;
}

struct ffn6_lookup_entry *__ffn6_get(const u32 *sip, const u32 *dip,
				     u16 sport, u16 dport,
				     int is_tcp)
{
	unsigned int hash;
	u8 protocol;

	/* lookup entry in cache */
	protocol = (is_tcp) ? IPPROTO_TCP : IPPROTO_UDP;
	hash = ffn6_hash(sip, dip, sport, dport, is_tcp);
	return __ffn6_find(sip, dip, sport, dport, protocol, hash);
}

static int __ffn6_add_entry(struct ffn6_lookup_entry *e,
			    u8 proto, unsigned int hash)
{
	/* make sure it's not present */
	if (__ffn6_find(e->sip, e->dip, e->sport, e->dport, proto, hash))
		return 1;

	if (ffn6_entry_count >= MAX_FFN6_ENTRY)
		return 1;

	/* add new entry */
	list_add_tail(&e->next, &ffn6_cache[hash % FFN6_CACHE_SIZE]);
	list_add_tail(&e->all_next, &ffn6_all);
	ffn6_entry_count++;
	return 0;
}

/*
 * two hooks into netfilter code
 */
extern int external_tcpv6_packet(struct nf_conn *ct,
				 const struct sk_buff *skb,
				 unsigned int dataoff,
				 enum ip_conntrack_info ctinfo);

extern int external_udpv6_packet(struct nf_conn *ct,
				 const struct sk_buff *skb,
				 unsigned int dataoff,
				 enum ip_conntrack_info ctinfo);

/*
 * check if packet is in ffn cache, or mark it if it can be added
 * later
 */
int ipv6_ffn_process(struct sk_buff *skb)
{
	struct ffn6_lookup_entry *e;
	struct nf_conntrack *nfct;
	struct ipv6hdr *iph;
	struct tcphdr *tcph = NULL;
	struct udphdr *udph = NULL;
	bool remove_me;
	int added_when;
	u8 proto;
	int res;

	if (!net_eq(dev_net(skb->dev), &init_net))
		goto not_ffnable;

	iph = ipv6_hdr(skb);

	/* check encapsulated protocol is udp or tcp */
	proto = iph->nexthdr;
	if (proto != IPPROTO_TCP && proto != IPPROTO_UDP)
		goto not_ffnable;

	if (iph->hop_limit <= 1 || !iph->payload_len)
		goto not_ffnable;

	/* TODO: implement this later, no hardware to test for now */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		goto not_ffnable;

	proto = iph->nexthdr;
	if (proto == IPPROTO_TCP) {
		if (skb_headlen(skb) < sizeof (*iph) + sizeof (struct tcphdr))
			goto not_ffnable;

		tcph = (struct tcphdr *)((unsigned char *)iph + sizeof (*iph));

		spin_lock_bh(&ffn6_lock);
		e = __ffn6_get(iph->saddr.s6_addr32, iph->daddr.s6_addr32,
			       tcph->source, tcph->dest, 1);
	} else {

		if (skb_headlen(skb) < sizeof (*iph) + sizeof (struct udphdr))
			goto not_ffnable;

		udph = (struct udphdr *)((unsigned char *)iph + sizeof (*iph));

		spin_lock_bh(&ffn6_lock);
		e = __ffn6_get(iph->saddr.s6_addr32, iph->daddr.s6_addr32,
			       udph->source, udph->dest, 0);
	}

	if (!e) {
		spin_unlock_bh(&ffn6_lock);
		goto ffnable;
	}

	if (e->manip.dst->obsolete > 0) {
		__ffn6_remove_entry(e);
		spin_unlock_bh(&ffn6_lock);
		goto ffnable;
	}

	nfct = &e->manip.ct->ct_general;
	nf_conntrack_get(nfct);

	remove_me = false;
	if (proto == IPPROTO_TCP) {
		/* do sequence number checking and update
		 * conntrack info */
		res = external_tcpv6_packet(e->manip.ct, skb, sizeof (*iph),
					    e->manip.ctinfo);
		if (e->manip.ct->proto.tcp.state != TCP_ESTABLISHED)
			remove_me = true;
	} else {
		res = external_udpv6_packet(e->manip.ct, skb, sizeof (*iph),
					    e->manip.ctinfo);
	}

	if (unlikely(res != NF_ACCEPT)) {
		/* packet rejected by conntrack, unless asked to drop,
		 * send it back into kernel */
		if (remove_me)
			__ffn6_remove_entry(e);

		spin_unlock_bh(&ffn6_lock);
		nf_conntrack_put(nfct);

		if (res == NF_DROP) {
			dev_kfree_skb(skb);
			return 0;
		}

		goto ffnable;
	}

	if (!e->manip.alter)
		goto fix_ip_hdr;

	/* fix ports & transport protocol checksum */
	if (proto == IPPROTO_TCP) {
		tcph->source = e->manip.new_sport;
		tcph->dest = e->manip.new_dport;
		tcph->check = csum16_sub(tcph->check, e->manip.adjustment);
	} else {
		udph->source = e->manip.new_sport;
		udph->dest = e->manip.new_dport;
		if (udph->check) {
			u16 tcheck;

			tcheck = csum16_sub(udph->check, e->manip.adjustment);
			udph->check = tcheck ? tcheck : 0xffff;
		}
	}

	memcpy(iph->saddr.s6_addr32, e->manip.new_sip, 16);
	memcpy(iph->daddr.s6_addr32, e->manip.new_dip, 16);

fix_ip_hdr:
	/* update IP header field */
	iph->hop_limit--;
	if (e->manip.tos_change)
		ipv6_change_dsfield(iph, 0, e->manip.new_tos);

	if (e->manip.force_skb_prio)
		skb->priority = e->manip.new_skb_prio;
	else
		skb->priority = rt_tos2priority(ipv6_get_dsfield(iph));

	skb->mark = e->manip.new_mark;

#ifdef CONFIG_IPV6_FFN_PROCFS
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
				    " sip: %pI6 -> %pI6\n"
				    " dip: %pI6 -> %pI6\n"
				    " sport: %u -> %u\n"
				    " dport: %u -> %u\n",
				    e->added_when,
				    e->sip, e->manip.new_sip,
				    e->dip, e->manip.new_dip,
				    htons(e->sport), htons(e->manip.new_sport),
				    htons(e->dport), htons(e->manip.new_dport));
			goto not_ffnable;
		}
		skb->nfct = nfct;
		skb->nfctinfo = e->manip.ctinfo;
	}

	added_when = e->added_when;
	if (remove_me)
		__ffn6_remove_entry(e);
	spin_unlock_bh(&ffn6_lock);

	skb->ffn_state = FFN_STATE_FAST_FORWARDED;

	if (added_when == IPV6_FFN_FINISH_OUT)
		dst_output(skb);
	else
		ip6_input_finish(skb->sk, skb);

	return 0;

ffnable:
	skb->ffn_state = FFN_STATE_FORWARDABLE;
	skb->ffn_orig_tos = ipv6_get_dsfield(iph);
	return 1;

not_ffnable:
	skb->ffn_state = FFN_STATE_INCOMPATIBLE;
	return 1;
}

/*
 *
 */
static inline __sum16 checksum_adjust(const u32 *osip,
				      const u32 *nsip,
				      const u32 *odip,
				      const u32 *ndip,
				      u16 osport,
				      u16 nsport,
				      u16 odport,
				      u16 ndport)
{
	const u32 oports[] = { osport, odport };
	const u32 nports[] = { nsport, ndport };
	__wsum osum, nsum;

	osum = csum_partial(osip, 16, 0);
	osum = csum_partial(odip, 16, osum);
	osum = csum_partial(oports, 8, osum);

	nsum = csum_partial(nsip, 16, 0);
	nsum = csum_partial(ndip, 16, nsum);
	nsum = csum_partial(nports, 8, nsum);

	return ~csum_fold(csum_sub(nsum, osum));
}

/*
 * check if skb is candidate for ffn, and if so add it to ffn cache
 *
 * called after post routing
 */
void ipv6_ffn_add(struct sk_buff *skb, int when)
{
	struct nf_conn *ct;
	struct nf_conntrack_tuple *tuple, *rtuple;
	enum ip_conntrack_info ctinfo;
	struct ffn6_lookup_entry *e;
	struct ipv6hdr *iph;
	unsigned int hash;
	int dir;
	u8 proto, tos;

	if (!net_eq(dev_net(skb->dev), &init_net))
		return;

	if (ffn6_entry_count >= MAX_FFN6_ENTRY)
		return;

	iph = ipv6_hdr(skb);

	if (!skb->nfct || (when == IPV6_FFN_FINISH_OUT &&
			   skb_dst(skb)->output != ip6_output))
		return;

	if (skb->nfct == &nf_ct_untracked_get()->ct_general)
		return;

	ct = nf_ct_get(skb, &ctinfo);
	if ((ctinfo != IP_CT_ESTABLISHED) &&
	    (ctinfo != IP_CT_ESTABLISHED + IP_CT_IS_REPLY))
		return;

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
	memcpy(e->sip, tuple->src.u3.ip6, 16);
	memcpy(e->dip, tuple->dst.u3.ip6, 16);
	e->sport = tuple->src.u.all;
	e->dport = tuple->dst.u.all;
	e->protocol = tuple->dst.protonum;

#ifdef CONFIG_IPV6_FFN_PROCFS
	e->forwarded_packets = 0;
	e->forwarded_bytes = 0;
#endif

	memcpy(e->manip.new_sip, rtuple->dst.u3.ip6, 16);
	memcpy(e->manip.new_dip, rtuple->src.u3.ip6, 16);
	e->manip.new_sport = rtuple->dst.u.all;
	e->manip.new_dport = rtuple->src.u.all;

	if (!memcmp(e->manip.new_sip, e->sip, 16) &&
	    !memcmp(e->manip.new_dip, e->dip, 16) &&
	    e->manip.new_sport == e->sport &&
	    e->manip.new_dport == e->dport)
		e->manip.alter = 0;
	else
		e->manip.alter = 1;

	if (e->manip.alter) {
		/* compute checksum adjustement */
		e->manip.adjustment = checksum_adjust(e->sip,
						      e->manip.new_sip,
						      e->dip,
						      e->manip.new_dip,
						      e->sport,
						      e->manip.new_sport,
						      e->dport,
						      e->manip.new_dport);
	}

	tos = ipv6_get_dsfield(iph);
	if (skb->ffn_orig_tos != tos) {
		e->manip.tos_change = 1;
		e->manip.new_tos = tos;
	} else
		e->manip.tos_change = 0;

	if (skb->priority != rt_tos2priority(tos)) {
		e->manip.force_skb_prio = 1;
		e->manip.new_skb_prio = skb->priority;
	} else
		e->manip.force_skb_prio = 0;

	e->manip.new_mark = skb->mark;
	e->manip.dst = skb_dst(skb);
	e->manip.priv_destructor = NULL;
	dst_hold(e->manip.dst);
	e->manip.ct = ct;
	e->manip.ctinfo = ctinfo;

	hash = ffn6_hash(e->sip, e->dip, e->sport, e->dport,
			 e->protocol == IPPROTO_TCP);
	proto = (e->protocol == IPPROTO_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

	spin_lock_bh(&ffn6_lock);
	if (__ffn6_add_entry(e, proto, hash)) {
		spin_unlock_bh(&ffn6_lock);
		dst_release(e->manip.dst);
		kfree(e);
		return;
	}
	spin_unlock_bh(&ffn6_lock);
}

/*
 * netfilter callback when conntrack is about to be destroyed
 */
void ipv6_ffn_ct_destroy(struct nf_conn *ct)
{
	struct nf_conntrack_tuple *tuple;
	struct ffn6_lookup_entry *e;
	int dir;

	/* locate all entry that use this conntrack */
	for (dir = 0; dir < 2; dir++) {
		tuple = &ct->tuplehash[dir].tuple;

		if (tuple->dst.protonum != IPPROTO_TCP &&
		    tuple->dst.protonum != IPPROTO_UDP)
			return;

		spin_lock_bh(&ffn6_lock);
		e = __ffn6_get(tuple->src.u3.ip6, tuple->dst.u3.ip6,
			       tuple->src.u.all, tuple->dst.u.all,
			       tuple->dst.protonum == IPPROTO_TCP);
		if (e)
			__ffn6_remove_entry(e);
		spin_unlock_bh(&ffn6_lock);
	}
}

/*
 * initialize ffn cache data
 */
static void __ipv6_ffn_init_cache(void)
{
	int i;

	for (i = 0; i < FFN6_CACHE_SIZE; i++)
		INIT_LIST_HEAD(&ffn6_cache[i]);
	INIT_LIST_HEAD(&ffn6_all);
	ffn6_entry_count = 0;
}

/*
 * flush all ffn cache
 */
void ipv6_ffn_flush_all(void)
{
	struct ffn6_lookup_entry *e, *tmp;

	spin_lock_bh(&ffn6_lock);
	list_for_each_entry_safe(e, tmp, &ffn6_all, all_next)
		__ffn6_remove_entry(e);
	__ipv6_ffn_init_cache();
	spin_unlock_bh(&ffn6_lock);
}

#ifdef CONFIG_IPV6_FFN_PROCFS
struct proc_dir_entry *proc_net_ipv6_ffn;

static int ipv6_ffn_entries_show(struct seq_file *m, void *v)
{
	int i;

	spin_lock_bh(&ffn6_lock);

	for (i = 0; i < FFN6_CACHE_SIZE; ++i) {
		struct ffn6_lookup_entry *e;

		if (list_empty(&ffn6_cache[i]))
			continue;

		seq_printf(m, "Bucket %i:\n", i);
		list_for_each_entry (e, &ffn6_cache[i], next) {
			seq_printf(m, " Protocol: ");
			switch (e->protocol) {
			case IPPROTO_TCP:
				seq_printf(m, "TCPv6\n");
				break;
			case IPPROTO_UDP:
				seq_printf(m, "UDPv6\n");
				break;
			default:
				seq_printf(m, "ipproto_%i\n", e->protocol);
				break;
			}

			seq_printf(m, " Original flow: %pI6:%u -> %pI6:%u\n",
				   e->sip,
				   ntohs(e->sport),
				   e->dip,
				   ntohs(e->dport));

			if (memcmp(e->sip, e->manip.new_sip, 16) ||
			    memcmp(e->dip, e->manip.new_dip, 16) ||
			    e->sport != e->manip.new_sport ||
			    e->dport != e->manip.new_dport) {
				seq_printf(m,
					   " Modified flow: %pI6:%u -> "
					   "%pI6:%u\n",
					   e->manip.new_sip,
					   ntohs(e->manip.new_sport),
					   e->manip.new_dip,
					   ntohs(e->manip.new_dport));
			}

			seq_printf(m, "  Forwarded packets: %u\n",
				   e->forwarded_packets);
			seq_printf(m, "  Forwarded bytes: %llu\n",
				   e->forwarded_bytes);
			seq_printf(m, "\n");
		}
	}

	spin_unlock_bh(&ffn6_lock);
	return 0;
}

static int ipv6_ffn_entries_open(struct inode *inode, struct file *file)
{
	return single_open(file, ipv6_ffn_entries_show, NULL);
}

static const struct file_operations ipv6_ffn_entries_fops = {
	.owner = THIS_MODULE,
	.open	= ipv6_ffn_entries_open,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek,
};


static int __init __ipv6_ffn_init_procfs(void)
{
	proc_net_ipv6_ffn = proc_net_mkdir(&init_net, "ipv6_ffn",
					 init_net.proc_net);
	if (!proc_net_ipv6_ffn) {
		printk(KERN_ERR "proc_mkdir() has failed "
		       "for 'net/ipv6_ffn'.\n");
		return -1;
	}

	if (proc_create("entries", 0400, proc_net_ipv6_ffn,
			&ipv6_ffn_entries_fops) == NULL) {
		printk(KERN_ERR "proc_create() has failed for "
		       "'net/ipv6_ffn/entries'.\n");
		return -1;
	}
	return 0;
}
#endif

/*
 * initialize ffn
 */
void __init ipv6_ffn_init(void)
{
	printk("IPv6 Fast Forward and NAT enabled\n");
	__ipv6_ffn_init_cache();

#ifdef CONFIG_IPV6_FFN_PROCFS
	if (__ipv6_ffn_init_procfs() < 0)
		printk(KERN_WARNING "IPv6 FFN: unable to create proc entries.\n");
#endif
}
