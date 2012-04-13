/*
 *	xt_hashlimit - Netfilter module to limit the number of packets per time
 *	seperately for each hashbucket (sourceip/sourceport/dstip/dstport)
 *
 *	(C) 2003-2004 by Harald Welte <laforge@netfilter.org>
 *	Copyright Â© CC Computer Consultants GmbH, 2007 - 2008
 *
 * Development of this code was funded by Astaro AG, http://www.astaro.com/
 */
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/mm.h>
#include <linux/in.h>
#include <linux/ip.h>
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
#include <linux/ipv6.h>
#include <net/ipv6.h>
#endif

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/netfilter/xt_hashlimit.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harald Welte <laforge@netfilter.org>");
MODULE_AUTHOR("Jan Engelhardt <jengelh@computergmbh.de>");
MODULE_DESCRIPTION("iptables match for limiting per hash-bucket");
MODULE_ALIAS("ipt_hashlimit");
MODULE_ALIAS("ip6t_hashlimit");

/* need to declare this at the top */
static struct proc_dir_entry *hashlimit_procdir4;
static struct proc_dir_entry *hashlimit_procdir6;
static const struct file_operations dl_file_ops;

/* hash table crap */
struct dsthash_dst {
	union {
		struct {
			__be32 src;
			__be32 dst;
		} ip;
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
		struct {
			__be32 src[4];
			__be32 dst[4];
		} ip6;
#endif
	};
	__be16 src_port;
	__be16 dst_port;
};

struct dsthash_ent {
	/* static / read-only parts in the beginning */
	struct hlist_node node;
	struct dsthash_dst dst;

	/* modified structure members in the end */
	unsigned long expires;		/* precalculated expiry time */
	struct {
		unsigned long prev;	/* last modification */
		u_int32_t credit;
		u_int32_t credit_cap, cost;
	} rateinfo;
};

struct xt_hashlimit_htable {
	struct hlist_node node;		/* global list of all htables */
	int use;
	int family;
	bool rnd_initialized;

	struct hashlimit_cfg1 cfg;	/* config */

	/* used internally */
	spinlock_t lock;		/* lock for list_head */
	u_int32_t rnd;			/* random seed for hash */
	unsigned int count;		/* number entries in table */
	struct timer_list timer;	/* timer for gc */

	/* seq_file stuff */
	struct proc_dir_entry *pde;

	struct hlist_head hash[0];	/* hashtable itself */
};

static DEFINE_MUTEX(hashlimit_mutex);	/* protects htables list */
static HLIST_HEAD(hashlimit_htables);
static struct kmem_cache *hashlimit_cachep __read_mostly;

static inline int dst_cmp(const struct dsthash_ent *ent, struct dsthash_dst *b)
{
	return !memcmp(&ent->dst, b, sizeof(ent->dst));
}

static u_int32_t
hash_dst(const struct xt_hashlimit_htable *ht, const struct dsthash_dst *dst)
{
	u_int32_t hash = jhash2((const u32 *)dst,
				sizeof(*dst)/sizeof(u32),
				ht->rnd);
	/*
	 * Instead of returning hash % ht->cfg.size (implying a divide)
	 * we return the high 32 bits of the (hash * ht->cfg.size) that will
	 * give results between [0 and cfg.size-1] and same hash distribution,
	 * but using a multiply, less expensive than a divide
	 */
	return ((u64)hash * ht->cfg.size) >> 32;
}

static struct dsthash_ent *
dsthash_find(const struct xt_hashlimit_htable *ht, struct dsthash_dst *dst)
{
	struct dsthash_ent *ent;
	struct hlist_node *pos;
	u_int32_t hash = hash_dst(ht, dst);

	if (!hlist_empty(&ht->hash[hash])) {
		hlist_for_each_entry(ent, pos, &ht->hash[hash], node)
			if (dst_cmp(ent, dst))
				return ent;
	}
	return NULL;
}

/* allocate dsthash_ent, initialize dst, put in htable and lock it */
static struct dsthash_ent *
dsthash_alloc_init(struct xt_hashlimit_htable *ht, struct dsthash_dst *dst)
{
	struct dsthash_ent *ent;

	/* initialize hash with random val at the time we allocate
	 * the first hashtable entry */
	if (!ht->rnd_initialized) {
		get_random_bytes(&ht->rnd, sizeof(ht->rnd));
		ht->rnd_initialized = true;
	}

	if (ht->cfg.max && ht->count >= ht->cfg.max) {
		/* FIXME: do something. question is what.. */
		if (net_ratelimit())
			printk(KERN_WARNING
				"xt_hashlimit: max count of %u reached\n",
				ht->cfg.max);
		return NULL;
	}

	ent = kmem_cache_alloc(hashlimit_cachep, GFP_ATOMIC);
	if (!ent) {
		if (net_ratelimit())
			printk(KERN_ERR
				"xt_hashlimit: can't allocate dsthash_ent\n");
		return NULL;
	}
	memcpy(&ent->dst, dst, sizeof(ent->dst));

	hlist_add_head(&ent->node, &ht->hash[hash_dst(ht, dst)]);
	ht->count++;
	return ent;
}

static inline void
dsthash_free(struct xt_hashlimit_htable *ht, struct dsthash_ent *ent)
{
	hlist_del(&ent->node);
	kmem_cache_free(hashlimit_cachep, ent);
	ht->count--;
}
static void htable_gc(unsigned long htlong);

static int htable_create(struct xt_hashlimit_mtinfo1 *minfo,
                         unsigned int family)
{
	struct xt_hashlimit_htable *hinfo;
	unsigned int size;
	unsigned int i;

	if (minfo->cfg.size) {
		size = minfo->cfg.size;
	} else {
		size = (num_physpages << PAGE_SHIFT) / 16384 /
		       sizeof(struct list_head);
		if (num_physpages > 1024 * 1024 * 1024 / PAGE_SIZE)
			size = 8192;
		if (size < 16)
			size = 16;
	}
	/* FIXME: don't use vmalloc() here or anywhere else -HW */
	hinfo = vmalloc(sizeof(struct xt_hashlimit_htable) +
	                sizeof(struct list_head) * size);
	if (hinfo == NULL) {
		printk(KERN_ERR "xt_hashlimit: unable to create hashtable\n");
		return -1;
	}
	minfo->hinfo = hinfo;

	/* copy match config into hashtable config */
	memcpy(&hinfo->cfg, &minfo->cfg, sizeof(hinfo->cfg));
	hinfo->cfg.size = size;
	if (hinfo->cfg.max == 0)
		hinfo->cfg.max = 8 * hinfo->cfg.size;
	else if (hinfo->cfg.max < hinfo->cfg.size)
		hinfo->cfg.max = hinfo->cfg.size;

	for (i = 0; i < hinfo->cfg.size; i++)
		INIT_HLIST_HEAD(&hinfo->hash[i]);

	hinfo->use = 1;
	hinfo->count = 0;
	hinfo->family = family;
	hinfo->rnd_initialized = false;
	spin_lock_init(&hinfo->lock);

	hinfo->pde = create_proc_entry(minfo->name, 0,
				       family == AF_INET ? hashlimit_procdir4 :
							   hashlimit_procdir6);
	if (hinfo->pde == NULL) {
		vfree(hinfo);
		return -1;
	}
	hinfo->pde->proc_fops = &dl_file_ops;
	hinfo->pde->data = hinfo;

	setup_timer(&hinfo->timer, htable_gc, (unsigned long)hinfo);
	hinfo->timer.expires = jiffies + msecs_to_jiffies(hinfo->cfg.gc_interval);
	add_timer(&hinfo->timer);

	hlist_add_head(&hinfo->node, &hashlimit_htables);

	return 0;
}

static int select_all(struct xt_hashlimit_htable *ht, struct dsthash_ent *he)
{
	return 1;
}

static int select_gc(struct xt_hashlimit_htable *ht, struct dsthash_ent *he)
{
	return time_after_eq(jiffies, he->expires);
}

static void htable_selective_cleanup(struct xt_hashlimit_htable *ht,
				int (*select)(struct xt_hashlimit_htable *ht,
					      struct dsthash_ent *he))
{
	unsigned int i;

	/* lock hash table and iterate over it */
	spin_lock_bh(&ht->lock);
	for (i = 0; i < ht->cfg.size; i++) {
		struct dsthash_ent *dh;
		struct hlist_node *pos, *n;
		hlist_for_each_entry_safe(dh, pos, n, &ht->hash[i], node) {
			if ((*select)(ht, dh))
				dsthash_free(ht, dh);
		}
	}
	spin_unlock_bh(&ht->lock);
}

/* hash table garbage collector, run by timer */
static void htable_gc(unsigned long htlong)
{
	struct xt_hashlimit_htable *ht = (struct xt_hashlimit_htable *)htlong;

	htable_selective_cleanup(ht, select_gc);

	/* re-add the timer accordingly */
	ht->timer.expires = jiffies + msecs_to_jiffies(ht->cfg.gc_interval);
	add_timer(&ht->timer);
}

static void htable_destroy(struct xt_hashlimit_htable *hinfo)
{
	del_timer_sync(&hinfo->timer);

	/* remove proc entry */
	remove_proc_entry(hinfo->pde->name,
			  hinfo->family == AF_INET ? hashlimit_procdir4 :
						     hashlimit_procdir6);
	htable_selective_cleanup(hinfo, select_all);
	vfree(hinfo);
}

static struct xt_hashlimit_htable *htable_find_get(char *name, int family)
{
	struct xt_hashlimit_htable *hinfo;
	struct hlist_node *pos;

	hlist_for_each_entry(hinfo, pos, &hashlimit_htables, node) {
		if (!strcmp(name, hinfo->pde->name) &&
		    hinfo->family == family) {
			hinfo->use++;
			return hinfo;
		}
	}
	return NULL;
}

static void htable_put(struct xt_hashlimit_htable *hinfo)
{
	mutex_lock(&hashlimit_mutex);
	if (--hinfo->use == 0) {
		hlist_del(&hinfo->node);
		htable_destroy(hinfo);
	}
	mutex_unlock(&hashlimit_mutex);
}

/* The algorithm used is the Simple Token Bucket Filter (TBF)
 * see net/sched/sch_tbf.c in the linux source tree
 */

/* Rusty: This is my (non-mathematically-inclined) understanding of
   this algorithm.  The `average rate' in jiffies becomes your initial
   amount of credit `credit' and the most credit you can ever have
   `credit_cap'.  The `peak rate' becomes the cost of passing the
   test, `cost'.

   `prev' tracks the last packet hit: you gain one credit per jiffy.
   If you get credit balance more than this, the extra credit is
   discarded.  Every time the match passes, you lose `cost' credits;
   if you don't have that many, the test fails.

   See Alexey's formal explanation in net/sched/sch_tbf.c.

   To get the maximum range, we multiply by this factor (ie. you get N
   credits per jiffy).  We want to allow a rate as low as 1 per day
   (slowest userspace tool allows), which means
   CREDITS_PER_JIFFY*HZ*60*60*24 < 2^32 ie.
*/
#define MAX_CPJ (0xFFFFFFFF / (HZ*60*60*24))

/* Repeated shift and or gives us all 1s, final shift and add 1 gives
 * us the power of 2 below the theoretical max, so GCC simply does a
 * shift. */
#define _POW2_BELOW2(x) ((x)|((x)>>1))
#define _POW2_BELOW4(x) (_POW2_BELOW2(x)|_POW2_BELOW2((x)>>2))
#define _POW2_BELOW8(x) (_POW2_BELOW4(x)|_POW2_BELOW4((x)>>4))
#define _POW2_BELOW16(x) (_POW2_BELOW8(x)|_POW2_BELOW8((x)>>8))
#define _POW2_BELOW32(x) (_POW2_BELOW16(x)|_POW2_BELOW16((x)>>16))
#define POW2_BELOW32(x) ((_POW2_BELOW32(x)>>1) + 1)

#define CREDITS_PER_JIFFY POW2_BELOW32(MAX_CPJ)

/* Precision saver. */
static inline u_int32_t
user2credits(u_int32_t user)
{
	/* If multiplying would overflow... */
	if (user > 0xFFFFFFFF / (HZ*CREDITS_PER_JIFFY))
		/* Divide first. */
		return (user / XT_HASHLIMIT_SCALE) * HZ * CREDITS_PER_JIFFY;

	return (user * HZ * CREDITS_PER_JIFFY) / XT_HASHLIMIT_SCALE;
}

static inline void rateinfo_recalc(struct dsthash_ent *dh, unsigned long now)
{
	dh->rateinfo.credit += (now - dh->rateinfo.prev) * CREDITS_PER_JIFFY;
	if (dh->rateinfo.credit > dh->rateinfo.credit_cap)
		dh->rateinfo.credit = dh->rateinfo.credit_cap;
	dh->rateinfo.prev = now;
}

static inline __be32 maskl(__be32 a, unsigned int l)
{
	return l ? htonl(ntohl(a) & ~0 << (32 - l)) : 0;
}

#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
static void hashlimit_ipv6_mask(__be32 *i, unsigned int p)
{
	switch (p) {
	case 0 ... 31:
		i[0] = maskl(i[0], p);
		i[1] = i[2] = i[3] = 0;
		break;
	case 32 ... 63:
		i[1] = maskl(i[1], p - 32);
		i[2] = i[3] = 0;
		break;
	case 64 ... 95:
		i[2] = maskl(i[2], p - 64);
		i[3] = 0;
		break;
	case 96 ... 127:
		i[3] = maskl(i[3], p - 96);
		break;
	case 128:
		break;
	}
}
#endif

static int
hashlimit_init_dst(struct xt_hashlimit_htable *hinfo, struct dsthash_dst *dst,
		   const struct sk_buff *skb, unsigned int protoff)
{
	__be16 _ports[2], *ports;
	u8 nexthdr;

	memset(dst, 0, sizeof(*dst));

	switch (hinfo->family) {
	case AF_INET:
		if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_DIP)
			dst->ip.dst = maskl(ip_hdr(skb)->daddr,
			              hinfo->cfg.dstmask);
		if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_SIP)
			dst->ip.src = maskl(ip_hdr(skb)->saddr,
			              hinfo->cfg.srcmask);

		if (!(hinfo->cfg.mode &
		      (XT_HASHLIMIT_HASH_DPT | XT_HASHLIMIT_HASH_SPT)))
			return 0;
		nexthdr = ip_hdr(skb)->protocol;
		break;
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	case AF_INET6:
		if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_DIP) {
			memcpy(&dst->ip6.dst, &ipv6_hdr(skb)->daddr,
			       sizeof(dst->ip6.dst));
			hashlimit_ipv6_mask(dst->ip6.dst, hinfo->cfg.dstmask);
		}
		if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_SIP) {
			memcpy(&dst->ip6.src, &ipv6_hdr(skb)->saddr,
			       sizeof(dst->ip6.src));
			hashlimit_ipv6_mask(dst->ip6.src, hinfo->cfg.srcmask);
		}

		if (!(hinfo->cfg.mode &
		      (XT_HASHLIMIT_HASH_DPT | XT_HASHLIMIT_HASH_SPT)))
			return 0;
		nexthdr = ipv6_hdr(skb)->nexthdr;
		protoff = ipv6_skip_exthdr(skb, sizeof(struct ipv6hdr), &nexthdr);
		if ((int)protoff < 0)
			return -1;
		break;
#endif
	default:
		BUG();
		return 0;
	}

	switch (nexthdr) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
	case IPPROTO_SCTP:
	case IPPROTO_DCCP:
		ports = skb_header_pointer(skb, protoff, sizeof(_ports),
					   &_ports);
		break;
	default:
		_ports[0] = _ports[1] = 0;
		ports = _ports;
		break;
	}
	if (!ports)
		return -1;
	if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_SPT)
		dst->src_port = ports[0];
	if (hinfo->cfg.mode & XT_HASHLIMIT_HASH_DPT)
		dst->dst_port = ports[1];
	return 0;
}

static bool
hashlimit_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_hashlimit_mtinfo1 *info = par->matchinfo;
	struct xt_hashlimit_htable *hinfo = info->hinfo;
	unsigned long now = jiffies;
	struct dsthash_ent *dh;
	struct dsthash_dst dst;

	if (hashlimit_init_dst(hinfo, &dst, skb, par->thoff) < 0)
		goto hotdrop;

	spin_lock_bh(&hinfo->lock);
	dh = dsthash_find(hinfo, &dst);
	if (dh == NULL) {
		dh = dsthash_alloc_init(hinfo, &dst);
		if (dh == NULL) {
			spin_unlock_bh(&hinfo->lock);
			goto hotdrop;
		}

		dh->expires = jiffies + msecs_to_jiffies(hinfo->cfg.expire);
		dh->rateinfo.prev = jiffies;
		dh->rateinfo.credit = user2credits(hinfo->cfg.avg *
		                      hinfo->cfg.burst);
		dh->rateinfo.credit_cap = user2credits(hinfo->cfg.avg *
		                          hinfo->cfg.burst);
		dh->rateinfo.cost = user2credits(hinfo->cfg.avg);
	} else {
		/* update expiration timeout */
		dh->expires = now + msecs_to_jiffies(hinfo->cfg.expire);
		rateinfo_recalc(dh, now);
	}

	if (dh->rateinfo.credit >= dh->rateinfo.cost) {
		/* below the limit */
		dh->rateinfo.credit -= dh->rateinfo.cost;
		spin_unlock_bh(&hinfo->lock);
		return !(info->cfg.mode & XT_HASHLIMIT_INVERT);
	}

	spin_unlock_bh(&hinfo->lock);
	/* default match is underlimit - so over the limit, we need to invert */
	return info->cfg.mode & XT_HASHLIMIT_INVERT;

 hotdrop:
	par->hotdrop = true;
	return false;
}

static bool hashlimit_mt_check(const struct xt_mtchk_param *par)
{
	struct xt_hashlimit_mtinfo1 *info = par->matchinfo;

	/* Check for overflow. */
	if (info->cfg.burst == 0 ||
	    user2credits(info->cfg.avg * info->cfg.burst) <
	    user2credits(info->cfg.avg)) {
		printk(KERN_ERR "xt_hashlimit: overflow, try lower: %u/%u\n",
		       info->cfg.avg, info->cfg.burst);
		return 0;
	}
	if (info->cfg.gc_interval == 0 || info->cfg.expire == 0)
		return 0;
	if (info->name[sizeof(info->name)-1] != '\0')
		return 0;
	if (par->family == NFPROTO_IPV4) {
		if (info->cfg.srcmask > 32 || info->cfg.dstmask > 32)
			return false;
	} else {
		if (info->cfg.srcmask > 128 || info->cfg.dstmask > 128)
			return false;
	}

	mutex_lock(&hashlimit_mutex);
	info->hinfo = htable_find_get(info->name, par->family);
	if (!info->hinfo && htable_create(info, par->family) != 0) {
		mutex_unlock(&hashlimit_mutex);
		return 0;
	}
	mutex_unlock(&hashlimit_mutex);
	return 1;
}

static void hashlimit_mt_destroy(const struct xt_mtdtor_param *par)
{
	const struct xt_hashlimit_mtinfo1 *info = par->matchinfo;

	htable_put(info->hinfo);
}

#ifdef CONFIG_COMPAT
struct compat_xt_hashlimit_info {
	char name[IFNAMSIZ];
	struct hashlimit_cfg cfg;
	compat_uptr_t hinfo;
	compat_uptr_t master;
};

static void compat_from_user(void *dst, void *src)
{
	int off = offsetof(struct compat_xt_hashlimit_info, hinfo);

	memcpy(dst, src, off);
	memset(dst + off, 0, sizeof(struct compat_xt_hashlimit_info) - off);
}

static int compat_to_user(void __user *dst, void *src)
{
	int off = offsetof(struct compat_xt_hashlimit_info, hinfo);

	return copy_to_user(dst, src, off) ? -EFAULT : 0;
}
#endif

static struct xt_match xt_hashlimit[] __read_mostly = {
	{
		.name           = "hashlimit",
		.revision       = 1,
		.family         = AF_INET,
		.match          = hashlimit_mt,
		.matchsize      = sizeof(struct xt_hashlimit_mtinfo1),
		.checkentry     = hashlimit_mt_check,
		.destroy        = hashlimit_mt_destroy,
		.me             = THIS_MODULE,
	},
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	{
		.name           = "hashlimit",
		.revision       = 1,
		.family         = AF_INET6,
		.match          = hashlimit_mt,
		.matchsize      = sizeof(struct xt_hashlimit_mtinfo1),
		.checkentry     = hashlimit_mt_check,
		.destroy        = hashlimit_mt_destroy,
		.me             = THIS_MODULE,
	},
#endif
};

/* PROC stuff */
static void *dl_seq_start(struct seq_file *s, loff_t *pos)
	__acquires(htable->lock)
{
	struct xt_hashlimit_htable *htable = s->private;
	unsigned int *bucket;

	spin_lock_bh(&htable->lock);
	if (*pos >= htable->cfg.size)
		return NULL;

	bucket = kmalloc(sizeof(unsigned int), GFP_ATOMIC);
	if (!bucket)
		return ERR_PTR(-ENOMEM);

	*bucket = *pos;
	return bucket;
}

static void *dl_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct xt_hashlimit_htable *htable = s->private;
	unsigned int *bucket = (unsigned int *)v;

	*pos = ++(*bucket);
	if (*pos >= htable->cfg.size) {
		kfree(v);
		return NULL;
	}
	return bucket;
}

static void dl_seq_stop(struct seq_file *s, void *v)
	__releases(htable->lock)
{
	struct xt_hashlimit_htable *htable = s->private;
	unsigned int *bucket = (unsigned int *)v;

	if (!IS_ERR(bucket))
		kfree(bucket);
	spin_unlock_bh(&htable->lock);
}

static int dl_seq_real_show(struct dsthash_ent *ent, int family,
				   struct seq_file *s)
{
	/* recalculate to show accurate numbers */
	rateinfo_recalc(ent, jiffies);

	switch (family) {
	case AF_INET:
		return seq_printf(s, "%ld %u.%u.%u.%u:%u->"
				     "%u.%u.%u.%u:%u %u %u %u\n",
				 (long)(ent->expires - jiffies)/HZ,
				 NIPQUAD(ent->dst.ip.src),
				 ntohs(ent->dst.src_port),
				 NIPQUAD(ent->dst.ip.dst),
				 ntohs(ent->dst.dst_port),
				 ent->rateinfo.credit, ent->rateinfo.credit_cap,
				 ent->rateinfo.cost);
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	case AF_INET6:
		return seq_printf(s, "%ld " NIP6_FMT ":%u->"
				     NIP6_FMT ":%u %u %u %u\n",
				 (long)(ent->expires - jiffies)/HZ,
				 NIP6(*(struct in6_addr *)&ent->dst.ip6.src),
				 ntohs(ent->dst.src_port),
				 NIP6(*(struct in6_addr *)&ent->dst.ip6.dst),
				 ntohs(ent->dst.dst_port),
				 ent->rateinfo.credit, ent->rateinfo.credit_cap,
				 ent->rateinfo.cost);
#endif
	default:
		BUG();
		return 0;
	}
}

static int dl_seq_show(struct seq_file *s, void *v)
{
	struct xt_hashlimit_htable *htable = s->private;
	unsigned int *bucket = (unsigned int *)v;
	struct dsthash_ent *ent;
	struct hlist_node *pos;

	if (!hlist_empty(&htable->hash[*bucket])) {
		hlist_for_each_entry(ent, pos, &htable->hash[*bucket], node)
			if (dl_seq_real_show(ent, htable->family, s))
				return -1;
	}
	return 0;
}

static struct seq_operations dl_seq_ops = {
	.start = dl_seq_start,
	.next  = dl_seq_next,
	.stop  = dl_seq_stop,
	.show  = dl_seq_show
};

static int dl_proc_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &dl_seq_ops);

	if (!ret) {
		struct seq_file *sf = file->private_data;
		sf->private = PDE(inode)->data;
	}
	return ret;
}

static const struct file_operations dl_file_ops = {
	.owner   = THIS_MODULE,
	.open    = dl_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static int __init xt_hashlimit_init(void)
{
	int err;

	err = xt_register_matches(xt_hashlimit, ARRAY_SIZE(xt_hashlimit));
	if (err < 0)
		goto err1;

	err = -ENOMEM;
	hashlimit_cachep = kmem_cache_create("xt_hashlimit",
					    sizeof(struct dsthash_ent), 0, 0,
					    NULL);
	if (!hashlimit_cachep) {
		printk(KERN_ERR "xt_hashlimit: unable to create slab cache\n");
		goto err2;
	}
	hashlimit_procdir4 = proc_mkdir("ipt_hashlimit", proc_net);
	if (!hashlimit_procdir4) {
		printk(KERN_ERR "xt_hashlimit: unable to create proc dir "
				"entry\n");
		goto err3;
	}
	err = 0;
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	hashlimit_procdir6 = proc_mkdir("ip6t_hashlimit", proc_net);
	if (!hashlimit_procdir6) {
		printk(KERN_ERR "xt_hashlimit: unable to create proc dir "
				"entry\n");
		err = -ENOMEM;
	}
#endif
	if (!err)
		return 0;
	remove_proc_entry("ipt_hashlimit", proc_net);
err3:
	kmem_cache_destroy(hashlimit_cachep);
err2:
	xt_unregister_matches(xt_hashlimit, ARRAY_SIZE(xt_hashlimit));
err1:
	return err;

}

static void __exit xt_hashlimit_fini(void)
{
	remove_proc_entry("ipt_hashlimit", proc_net);
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	remove_proc_entry("ip6t_hashlimit", proc_net);
#endif
	kmem_cache_destroy(hashlimit_cachep);
	xt_unregister_matches(xt_hashlimit, ARRAY_SIZE(xt_hashlimit));
}

module_init(xt_hashlimit_init);
module_exit(xt_hashlimit_fini);
