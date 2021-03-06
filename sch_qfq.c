/*
 * net/sched/sch_qfq.c         Quick Fair Queueing Scheduler.
 *
 * Copyright (c) 2009 Fabio Checconi, Luigi Rizzo, and Paolo Valente.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/pkt_sched.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <trace/events/net.h>

#include <linux/kernel.h>
#include <linux/sched/rt.h>
#include <linux/kthread.h>
#include <net/sock.h>

/*  Quick Fair Queueing
    ===================

    Sources:

    Fabio Checconi, Luigi Rizzo, and Paolo Valente: "QFQ: Efficient
    Packet Scheduling with Tight Bandwidth Distribution Guarantees."

    See also:
    http://retis.sssup.it/~fabio/linux/qfq/
 */

/*

  Virtual time computations.

  S, F and V are all computed in fixed point arithmetic with
  FRAC_BITS decimal bits.

  QFQ_MAX_INDEX is the maximum index allowed for a group. We need
	one bit per index.
  QFQ_MAX_WSHIFT is the maximum power of two supported as a weight.

  The layout of the bits is as below:

                   [ MTU_SHIFT ][      FRAC_BITS    ]
                   [ MAX_INDEX    ][ MIN_SLOT_SHIFT ]
				 ^.__grp->index = 0
				 *.__grp->slot_shift

  where MIN_SLOT_SHIFT is derived by difference from the others.

  The max group index corresponds to Lmax/w_min, where
  Lmax=1<<MTU_SHIFT, w_min = 1 .
  From this, and knowing how many groups (MAX_INDEX) we want,
  we can derive the shift corresponding to each group.

  Because we often need to compute
	F = S + len/w_i  and V = V + len/wsum
  instead of storing w_i store the value
	inv_w = (1<<FRAC_BITS)/w_i
  so we can do F = S + len * inv_w * wsum.
  We use W_TOT in the formulas so we can easily move between
  static and adaptive weight sum.

  The per-scheduler-instance data contain all the data structures
  for the scheduler: bitmaps and bucket lists.

 */

/*
 * Maximum number of consecutive slots occupied by backlogged classes
 * inside a group.
 */
#define QFQ_MAX_SLOTS	32

/*
 * Shifts used for class<->group mapping.  We allow class weights that are
 * in the range [1, 2^MAX_WSHIFT], and we try to map each class i to the
 * group with the smallest index that can support the L_i / r_i configured
 * for the class.
 *
 * grp->index is the index of the group; and grp->slot_shift
 * is the shift for the corresponding (scaled) sigma_i.
 */
#define QFQ_MAX_INDEX		19
#define QFQ_MAX_WSHIFT		16

#define	QFQ_MAX_WEIGHT		(1<<QFQ_MAX_WSHIFT)
#define QFQ_MAX_WSUM		(2*QFQ_MAX_WEIGHT)

#define FRAC_BITS		30	/* fixed point arithmetic */
#define ONE_FP			(1UL << FRAC_BITS)
#define IWSUM			(ONE_FP/QFQ_MAX_WSUM)

#define QFQ_MTU_SHIFT		11
#define QFQ_MIN_SLOT_SHIFT	(FRAC_BITS + QFQ_MTU_SHIFT - QFQ_MAX_INDEX)

/*
 * Link speed in Mbps. System time V will be incremented at this rate and the
 * rate limits of flows (still using the weight variable) should be also
 * indicated in Mbps.
 *
 * This value should actually be about 9844Mb/s but we leave it at
 * 9800 with the hope of having small queues in the NIC.  The reason
 * is that with a given MTU, each packet has an Ethernet preamble (4B)
 * and the frame check sequence (8B) and a minimum recommended
 * inter-packet gap (0.0096us for 10GbE = 12B).  Thus the max
 * achievable data rate is MTU / (MTU + 24), which is 0.98439 with MTU
 * = 1500B and and 0.99734 with MTU=9000B.
 */
#define LINK_SPEED		9800	// 10Gbps link
#define QFQ_DRAIN_RATE		((u64)LINK_SPEED * 125000 * ONE_FP / NSEC_PER_SEC)

static int spin_cpu = 2;
/* Module parameter and sysfs export */
module_param    (spin_cpu, int, 0640);
MODULE_PARM_DESC(spin_cpu, "CPU to spin on. Ensure no processes are scheduled here to minimise context switches.");

/*
 * Possible group states.  These values are used as indexes for the bitmaps
 * array of struct qfq_queue.
 */
enum qfq_state { ER, IR, EB, IB, QFQ_MAX_STATE };

struct qfq_group;

struct qfq_class {
	struct Qdisc_class_common common;

	unsigned int refcnt;
	unsigned int filter_cnt;

	struct gnet_stats_basic_packed bstats;
	struct gnet_stats_queue qstats;
	struct gnet_stats_rate_est rate_est;
	struct Qdisc *qdisc;

	struct hlist_node next;	/* Link for the slot list. */
	u64 S, F;		/* flow timestamps (exact) */

	/* group we belong to. In principle we would need the index,
	 * which is log_2(lmax/weight), but we never reference it
	 * directly, only the group.
	 */
	struct qfq_group *grp;

	/* these are copied from the flowset. */
	u32	inv_w;		/* ONE_FP/weight */
	u32	lmax;		/* Max packet size for this flow. */

//	/* stats variables */
//	u64 idle_on_deq; /* Class was idle after a dequeue from this class */
//	s64 prev_dequeue_time_ns;
//	s64 inter_dequeue_time_ns;
//
//	s64 expected_inter_dequeue_time_ns;
//	s64 absdev_dequeue_time_ns;
};

struct qfq_group {
	u64 S, F;			/* group timestamps (approx). */
	unsigned int slot_shift;	/* Slot shift. */
	unsigned int index;		/* Group index. */
	unsigned int front;		/* Index of the front slot. */
	unsigned long full_slots;	/* non-empty slots */

	/* Array of RR lists of active classes. */
	struct hlist_head slots[QFQ_MAX_SLOTS];
};

struct qfq_sched {
	struct tcf_proto *filter_list;
	struct Qdisc_class_hash clhash;

	u64		V;		/* Precise virtual time. */
	u32		wsum;		/* weight sum */
	u32		wsum_active;	/* weight sum of active classes */

	unsigned long bitmaps[QFQ_MAX_STATE];	    /* Group bitmaps. */
	struct qfq_group groups[QFQ_MAX_INDEX + 1]; /* The groups. */

	struct task_struct *spinner;

	/* real time maintenance */
	u64		v_last_updated;	/* Time when V was last updated */
	u64		v_diff_sum;	/* Running count of how much V should be
					 * incremented by.
					 */
	u64		t_diff_sum;	/* Running count of time (in
					 * psched_ticks) over which V should be
					 * incremented by v_diff_sum.
					 */

//	/* stats variables */
//	u64	v_forwarded;	/* V was forward to match S of some group in
//				 * order to avoid a non work conserving
//				 * schedule
//				 */
//	u64	idle_on_deq; /* Qdisc was idle after a dequeue operation */
//	u64	update_grp_on_deq; /* Group needed update upon dequeue */
//	u64	txq_blocked; /* Interface frozen or stopped while trying to
//			      * transmit skb. For each skb dequeued from qdisc,
//			      * we increment this value at most once (not for
//			      * each time we retry).
//			      */

	/* Per CPU locking and queues */
	unsigned long work_bitmap; /* Indicates scheduled work on different
				    * CPUs. Bit i is set if CPU i has
				    * scheduled activation work.
				    */
	struct qfq_cpu_work_queue __percpu *work_queue; /* Per CPU work queues
							 * which indicate that
							 * some classes have to
							 * be activated on this
							 * CPU.
							 */
};

struct qfq_cpu_work_queue {
	struct list_head list; /* Head of the work queue */
	spinlock_t lock;
};

struct qfq_cpu_work_entry {
	struct qfq_class *cl; /* Class that has to be activated */
	unsigned int pkt_len; /* Length of enqueued packet */
	struct list_head list;
};

static struct qfq_class *qfq_find_class(struct Qdisc *sch, u32 classid)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct Qdisc_class_common *clc;

	clc = qdisc_class_find(&q->clhash, classid);
	if (clc == NULL)
		return NULL;
	return container_of(clc, struct qfq_class, common);
}

static void qfq_purge_queue(struct qfq_class *cl)
{
	unsigned int len = cl->qdisc->q.qlen;

	qdisc_reset(cl->qdisc);
	qdisc_tree_decrease_qlen(cl->qdisc, len);
}

static const struct nla_policy qfq_policy[TCA_QFQ_MAX + 1] = {
	[TCA_QFQ_WEIGHT] = { .type = NLA_U32 },
	[TCA_QFQ_LMAX] = { .type = NLA_U32 },
};

/*
 * Calculate a flow index, given its weight and maximum packet length.
 * index = log_2(maxlen/weight) but we need to apply the scaling.
 * This is used only once at flow creation.
 */
static int qfq_calc_index(u32 inv_w, unsigned int maxlen)
{
	u64 slot_size = (u64)maxlen * inv_w;
	unsigned long size_map;
	int index = 0;

	if (inv_w == ONE_FP + 1)
		goto out;

	size_map = slot_size >> QFQ_MIN_SLOT_SHIFT;
	if (!size_map)
		goto out;

	index = __fls(size_map) + 1;	/* basically a log_2 */
	index -= !(slot_size - (1ULL << (index + QFQ_MIN_SLOT_SHIFT - 1)));

	if (index < 0)
		index = 0;
out:
	pr_debug("qfq calc_index: W = %lu, L = %u, I = %d\n",
		 (unsigned long) ONE_FP/inv_w, maxlen, index);

	return index;
}

/* Length of the next packet (0 if the queue is empty). */
static unsigned int qdisc_peek_len(struct Qdisc *sch)
{
	struct sk_buff *skb;

	skb = sch->ops->peek(sch);
	return skb ? qdisc_pkt_len(skb) : 0;
}

static void qfq_deactivate_class(struct qfq_sched *, struct qfq_class *);
static void qfq_activate_class(struct qfq_sched *q, struct qfq_class *cl,
			       unsigned int len);

static void qfq_update_class_params(struct qfq_sched *q, struct qfq_class *cl,
				    u32 lmax, u32 inv_w, int delta_w)
{
	int i;

	/* update qfq-specific data */
	cl->lmax = lmax;
	cl->inv_w = inv_w;
	i = qfq_calc_index(cl->inv_w, cl->lmax);

	cl->grp = &q->groups[i];

	q->wsum += delta_w;
}

static int qfq_change_class(struct Qdisc *sch, u32 classid, u32 parentid,
			    struct nlattr **tca, unsigned long *arg)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl = (struct qfq_class *)*arg;
	struct nlattr *tb[TCA_QFQ_MAX + 1];
	u32 weight, lmax, inv_w;
	int i, err;
	int delta_w;

	if (tca[TCA_OPTIONS] == NULL) {
		pr_notice("qfq: no options\n");
		return -EINVAL;
	}

	err = nla_parse_nested(tb, TCA_QFQ_MAX, tca[TCA_OPTIONS], qfq_policy);
	if (err < 0)
		return err;

	if (tb[TCA_QFQ_WEIGHT]) {
		weight = nla_get_u32(tb[TCA_QFQ_WEIGHT]);
		if (weight > (1UL << QFQ_MAX_WSHIFT)) {
			pr_notice("qfq: invalid weight %u\n", weight);
			return -EINVAL;
		}
	} else
		weight = 1;

	inv_w = weight ? ONE_FP / weight : ONE_FP + 1;
	weight = ONE_FP / inv_w;
	delta_w = weight - (cl ? ONE_FP / cl->inv_w : 0);
	if (q->wsum + delta_w > QFQ_MAX_WSUM) {
		pr_notice("qfq: total weight out of range (%u + %u)\n",
			  delta_w, q->wsum);
		return -EINVAL;
	}

	if (tb[TCA_QFQ_LMAX]) {
		lmax = nla_get_u32(tb[TCA_QFQ_LMAX]);
		if (!lmax || lmax > (1UL << QFQ_MTU_SHIFT)) {
			pr_notice("qfq: invalid max length %u\n", lmax);
			return -EINVAL;
		}
	} else
		lmax = 1UL << QFQ_MTU_SHIFT;

	if (cl != NULL) {
		bool need_reactivation = false;

		if (tca[TCA_RATE]) {
			err = gen_replace_estimator(&cl->bstats, &cl->rate_est,
						    qdisc_root_sleeping_lock(sch),
						    tca[TCA_RATE]);
			if (err)
				return err;
		}

		if (lmax == cl->lmax && inv_w == cl->inv_w)
			return 0; /* nothing to update */

		i = qfq_calc_index(inv_w, lmax);
		sch_tree_lock(sch);
		if (&q->groups[i] != cl->grp && cl->qdisc->q.qlen > 0 &&
		    cl->inv_w != ONE_FP + 1) {
			/*
			 * shift cl->F back, to not charge the
			 * class for the not-yet-served head
			 * packet
			 */
			cl->F = cl->S;
			/* remove class from its slot in the old group */
			qfq_deactivate_class(q, cl);
			if (inv_w != ONE_FP + 1)
				need_reactivation = true;
		}

		if (cl->inv_w == ONE_FP + 1 && inv_w != ONE_FP + 1)
			need_reactivation = true;

		qfq_update_class_params(q, cl, lmax, inv_w, delta_w);
		if (cl->qdisc->q.qlen > 0)
			q->wsum_active += delta_w;

		if (need_reactivation) /* activate in new group */
			qfq_activate_class(q, cl, qdisc_peek_len(cl->qdisc));
		sch_tree_unlock(sch);

		return 0;
	}

	cl = kzalloc(sizeof(struct qfq_class), GFP_KERNEL);
	if (cl == NULL)
		return -ENOBUFS;

	cl->refcnt = 1;
	cl->common.classid = classid;

	qfq_update_class_params(q, cl, lmax, inv_w, delta_w);

	cl->qdisc = qdisc_create_dflt(sch->dev_queue,
				      &pfifo_qdisc_ops, classid);
	if (cl->qdisc == NULL)
		cl->qdisc = &noop_qdisc;

	if (tca[TCA_RATE]) {
		err = gen_new_estimator(&cl->bstats, &cl->rate_est,
					qdisc_root_sleeping_lock(sch),
					tca[TCA_RATE]);
		if (err) {
			qdisc_destroy(cl->qdisc);
			kfree(cl);
			return err;
		}
	}

//	cl->prev_dequeue_time_ns = ktime_get().tv64;
//	cl->inter_dequeue_time_ns = 0;
//
//	cl->expected_inter_dequeue_time_ns = 1482LLU * 8 * 1000 / weight;
//	cl->absdev_dequeue_time_ns = 0;

	sch_tree_lock(sch);
	qdisc_class_hash_insert(&q->clhash, &cl->common);
	sch_tree_unlock(sch);

	qdisc_class_hash_grow(sch, &q->clhash);

	*arg = (unsigned long)cl;
	return 0;
}

static void qfq_destroy_class(struct Qdisc *sch, struct qfq_class *cl)
{
	struct qfq_sched *q = qdisc_priv(sch);

	if (cl->inv_w) {
		q->wsum -= ONE_FP / cl->inv_w;
		if (qdisc_qlen(cl->qdisc))
			q->wsum_active -= ONE_FP / cl->inv_w;
		cl->inv_w = 0;
	}

	gen_kill_estimator(&cl->bstats, &cl->rate_est);
	qdisc_destroy(cl->qdisc);
	kfree(cl);
}

static int qfq_delete_class(struct Qdisc *sch, unsigned long arg)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl = (struct qfq_class *)arg;

	if (cl->filter_cnt > 0)
		return -EBUSY;

	sch_tree_lock(sch);

	if (cl->inv_w && qdisc_qlen(cl->qdisc))
		q->wsum_active -= ONE_FP / cl->inv_w;

	qfq_purge_queue(cl);
	qdisc_class_hash_remove(&q->clhash, &cl->common);

	BUG_ON(--cl->refcnt == 0);
	/*
	 * This shouldn't happen: we "hold" one cops->get() when called
	 * from tc_ctl_tclass; the destroy method is done from cops->put().
	 */

	sch_tree_unlock(sch);
	return 0;
}

static unsigned long qfq_get_class(struct Qdisc *sch, u32 classid)
{
	struct qfq_class *cl = qfq_find_class(sch, classid);

	if (cl != NULL)
		cl->refcnt++;

	return (unsigned long)cl;
}

static void qfq_put_class(struct Qdisc *sch, unsigned long arg)
{
	struct qfq_class *cl = (struct qfq_class *)arg;

	if (--cl->refcnt == 0)
		qfq_destroy_class(sch, cl);
}

static struct tcf_proto **qfq_tcf_chain(struct Qdisc *sch, unsigned long cl)
{
	struct qfq_sched *q = qdisc_priv(sch);

	if (cl)
		return NULL;

	return &q->filter_list;
}

static unsigned long qfq_bind_tcf(struct Qdisc *sch, unsigned long parent,
				  u32 classid)
{
	struct qfq_class *cl = qfq_find_class(sch, classid);

	if (cl != NULL)
		cl->filter_cnt++;

	return (unsigned long)cl;
}

static void qfq_unbind_tcf(struct Qdisc *sch, unsigned long arg)
{
	struct qfq_class *cl = (struct qfq_class *)arg;

	cl->filter_cnt--;
}

static int qfq_graft_class(struct Qdisc *sch, unsigned long arg,
			   struct Qdisc *new, struct Qdisc **old)
{
	struct qfq_class *cl = (struct qfq_class *)arg;

	if (new == NULL) {
		new = qdisc_create_dflt(sch->dev_queue,
					&pfifo_qdisc_ops, cl->common.classid);
		if (new == NULL)
			new = &noop_qdisc;
	}

	sch_tree_lock(sch);
	qfq_purge_queue(cl);
	*old = cl->qdisc;
	cl->qdisc = new;
	sch_tree_unlock(sch);
	return 0;
}

static struct Qdisc *qfq_class_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct qfq_class *cl = (struct qfq_class *)arg;

	return cl->qdisc;
}

static int qfq_dump_class(struct Qdisc *sch, unsigned long arg,
			  struct sk_buff *skb, struct tcmsg *tcm)
{
	struct qfq_class *cl = (struct qfq_class *)arg;
	struct nlattr *nest;

	tcm->tcm_parent	= TC_H_ROOT;
	tcm->tcm_handle	= cl->common.classid;
	tcm->tcm_info	= cl->qdisc->handle;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (nest == NULL)
		goto nla_put_failure;
	if (nla_put_u32(skb, TCA_QFQ_WEIGHT, ONE_FP/cl->inv_w) ||
	    nla_put_u32(skb, TCA_QFQ_LMAX, cl->lmax))
		goto nla_put_failure;

	return nla_nest_end(skb, nest);

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int qfq_dump_class_stats(struct Qdisc *sch, unsigned long arg,
				struct gnet_dump *d)
{
	struct qfq_class *cl = (struct qfq_class *)arg;
	struct tc_qfq_xstats xstats = {.type = TCA_QFQ_XSTATS_CLASS};

//	xstats.class_stats.idle_on_deq = cl->idle_on_deq;
//	xstats.class_stats.inter_deq_time_ns = cl->inter_dequeue_time_ns;
//	xstats.class_stats.absdev_deq_time_ns = cl->absdev_dequeue_time_ns;
//	xstats.class_stats.expected_inter_dequeue_time_ns = cl->expected_inter_dequeue_time_ns;

	cl->qdisc->qstats.qlen = cl->qdisc->q.qlen;
	//printk(KERN_INFO "class %p inter_dequeue_time %lld\n", cl, cl->inter_dequeue_time_ns);

	if (gnet_stats_copy_basic(d, &cl->bstats) < 0 ||
	    gnet_stats_copy_rate_est(d, &cl->bstats, &cl->rate_est) < 0 ||
	    gnet_stats_copy_queue(d, &cl->qdisc->qstats) < 0)
		return -1;

	return gnet_stats_copy_app(d, &xstats, sizeof(xstats));
}

static void qfq_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl;
	unsigned int i;

	if (arg->stop)
		return;

	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &q->clhash.hash[i], common.hnode) {
			if (arg->count < arg->skip) {
				arg->count++;
				continue;
			}
			if (arg->fn(sch, (unsigned long)cl, arg) < 0) {
				arg->stop = 1;
				return;
			}
			arg->count++;
		}
	}
}

static struct qfq_class *qfq_classify(struct sk_buff *skb, struct Qdisc *sch,
				      int *qerr)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl;
	struct tcf_result res;
	int result;

	if (likely(skb->sk && skb->sk->qdisc_cache == sch && skb->sk->cl_cache)) {
		return skb->sk->cl_cache;
	}

	if (TC_H_MAJ(skb->priority ^ sch->handle) == 0) {
		pr_debug("qfq_classify: found %d\n", skb->priority);
		cl = qfq_find_class(sch, skb->priority);
		if (cl != NULL)
			return cl;
	}

	*qerr = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
	result = tc_classify(skb, q->filter_list, &res);
	if (result >= 0) {
#ifdef CONFIG_NET_CLS_ACT
		switch (result) {
		case TC_ACT_QUEUED:
		case TC_ACT_STOLEN:
			*qerr = NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
		case TC_ACT_SHOT:
			return NULL;
		}
#endif
		cl = (struct qfq_class *)res.class;
		if (cl == NULL)
			cl = qfq_find_class(sch, res.classid);
		return cl;
	}

	return NULL;
}

/* Generic comparison function, handling wraparound. */
static inline int qfq_gt(u64 a, u64 b)
{
	return (s64)(a - b) > 0;
}

/* Round a precise timestamp to its slotted value. */
static inline u64 qfq_round_down(u64 ts, unsigned int shift)
{
	return ts & ~((1ULL << shift) - 1);
}

/* return the pointer to the group with lowest index in the bitmap */
static inline struct qfq_group *qfq_ffs(struct qfq_sched *q,
					unsigned long bitmap)
{
	int index = __ffs(bitmap);
	return &q->groups[index];
}
/* Calculate a mask to mimic what would be ffs_from(). */
static inline unsigned long mask_from(unsigned long bitmap, int from)
{
	return bitmap & ~((1UL << from) - 1);
}

/*
 * The state computation relies on ER=0, IR=1, EB=2, IB=3
 * First compute eligibility comparing grp->S, q->V,
 * then check if someone is blocking us and possibly add EB
 */
static int qfq_calc_state(struct qfq_sched *q, const struct qfq_group *grp)
{
	/* if S > V we are not eligible */
	unsigned int state = qfq_gt(grp->S, q->V);
	unsigned long mask = mask_from(q->bitmaps[ER], grp->index);
	struct qfq_group *next;

	if (mask) {
		next = qfq_ffs(q, mask);
		if (qfq_gt(grp->F, next->F))
			state |= EB;
	}

	return state;
}


/*
 * In principle
 *	q->bitmaps[dst] |= q->bitmaps[src] & mask;
 *	q->bitmaps[src] &= ~mask;
 * but we should make sure that src != dst
 */
static inline void qfq_move_groups(struct qfq_sched *q, unsigned long mask,
				   int src, int dst)
{
	q->bitmaps[dst] |= q->bitmaps[src] & mask;
	q->bitmaps[src] &= ~mask;
}

static void qfq_unblock_groups(struct qfq_sched *q, int index, u64 old_F)
{
	unsigned long mask = mask_from(q->bitmaps[ER], index + 1);
	struct qfq_group *next;

	if (mask) {
		next = qfq_ffs(q, mask);
		if (!qfq_gt(next->F, old_F))
			return;
	}

	mask = (1UL << index) - 1;
	qfq_move_groups(q, mask, EB, ER);
	qfq_move_groups(q, mask, IB, IR);
}

/*
 * perhaps
 *
	old_V ^= q->V;
	old_V >>= QFQ_MIN_SLOT_SHIFT;
	if (old_V) {
		...
	}
 *
 */
static void qfq_make_eligible(struct qfq_sched *q, u64 old_V)
{
	unsigned long vslot = q->V >> QFQ_MIN_SLOT_SHIFT;
	unsigned long old_vslot = old_V >> QFQ_MIN_SLOT_SHIFT;

	if (vslot != old_vslot) {
		unsigned long mask = (1UL << fls(vslot ^ old_vslot)) - 1;
		qfq_move_groups(q, mask, IR, ER);
		qfq_move_groups(q, mask, IB, EB);
	}
}


/*
 * XXX we should make sure that slot becomes less than 32.
 * This is guaranteed by the input values.
 * roundedS is always cl->S rounded on grp->slot_shift bits.
 */
static void qfq_slot_insert(struct qfq_sched *q,
			    struct qfq_group *grp, struct qfq_class *cl,
			    u64 roundedS)
{
	u64 slot = (roundedS - grp->S) >> grp->slot_shift;
	unsigned int i = (grp->front + slot) % QFQ_MAX_SLOTS;

	if (unlikely(slot >= QFQ_MAX_SLOTS)) {
		printk_ratelimited(KERN_INFO "[%s...%pS] slot %llu "
				   "V=%llu "
				   "cl->S=%llu "
				   "roundedS=%llu "
				   "grp->S=%llu "
				   "grp->slot_shift=%u "
				   "grp->full_slots=0x%lx "
				   "grp->front=%u "
				   "grp->idx=%u "
				   "q->ER=0x%lx "
				   "q->EB=0x%lx "
				   "q->IR=0x%lx "
				   "q->IB=0x%lx\n",
				   __func__, __builtin_return_address(0),
				   slot, q->V, cl->S, roundedS, grp->S,
				   grp->slot_shift, grp->full_slots,
				   grp->front, grp->index, q->bitmaps[ER],
				   q->bitmaps[EB], q->bitmaps[IR],
				   q->bitmaps[IB]);
		slot = QFQ_MAX_SLOTS - 1;
		i = (grp->front + slot) % QFQ_MAX_SLOTS;
	}
	hlist_add_head(&cl->next, &grp->slots[i]);
	__set_bit(slot, &grp->full_slots);
}

/* Maybe introduce hlist_first_entry?? */
static struct qfq_class *qfq_slot_head(struct qfq_group *grp)
{
	return hlist_entry(grp->slots[grp->front].first,
			   struct qfq_class, next);
}

/*
 * remove the entry from the slot
 */
static void qfq_front_slot_remove(struct qfq_group *grp)
{
	struct qfq_class *cl = qfq_slot_head(grp);

	BUG_ON(!cl);
	hlist_del(&cl->next);
	if (hlist_empty(&grp->slots[grp->front]))
		__clear_bit(0, &grp->full_slots);
}

/*
 * Returns the first full queue in a group. As a side effect,
 * adjust the bucket list so the first non-empty bucket is at
 * position 0 in full_slots.
 */
static struct qfq_class *qfq_slot_scan(struct qfq_group *grp)
{
	unsigned int i;

	pr_debug("qfq slot_scan: grp %u full %#lx\n",
		 grp->index, grp->full_slots);

	if (grp->full_slots == 0)
		return NULL;

	i = __ffs(grp->full_slots);  /* zero based */
	if (i > 0) {
		grp->front = (grp->front + i) % QFQ_MAX_SLOTS;
		grp->full_slots >>= i;
	}

	return qfq_slot_head(grp);
}

/*
 * adjust the bucket list. When the start time of a group decreases,
 * we move the index down (modulo QFQ_MAX_SLOTS) so we don't need to
 * move the objects. The mask of occupied slots must be shifted
 * because we use ffs() to find the first non-empty slot.
 * This covers decreases in the group's start time, but what about
 * increases of the start time ?
 * Here too we should make sure that i is less than 32
 */
static void qfq_slot_rotate(struct qfq_group *grp, u64 roundedS)
{
	unsigned int i = (grp->S - roundedS) >> grp->slot_shift;

	grp->full_slots <<= i;
	grp->front = (grp->front - i) % QFQ_MAX_SLOTS;
}

static void qfq_update_eligible(struct qfq_sched *q, u64 old_V)
{
	unsigned long ineligible;

	ineligible = q->bitmaps[IR] | q->bitmaps[IB];
	if (ineligible) {
		/*
		 * For standard QFQ, we would first ensure V is not less
		 * than the start time of the next ineligible group (work
		 * conserving schedule) and update V if required.
		 */
		qfq_make_eligible(q, old_V);
	}
}

/*
 * Updates the class, returns true if also the group needs to be updated.
 */
static bool qfq_update_class(struct qfq_sched *q,
			     struct qfq_group *grp, struct qfq_class *cl,
			     unsigned int len)
{
	/* We do not hold the class lock while updating class variables such as
	 * S and F here. These variables are only updated from the dequeue
	 * thread and the enqueue operation only makes changes to the class
	 * qdisc.
	 */

	cl->S = cl->F;
	if (!len) {
		qfq_front_slot_remove(grp);	/* queue is empty */
		//cl->idle_on_deq++;
	} else if (cl->inv_w == ONE_FP + 1) {
		qfq_front_slot_remove(grp);	/* weight was changed to zero */
	} else {
		u64 roundedS;

		cl->F = cl->S + (u64)len * cl->inv_w;
		roundedS = qfq_round_down(cl->S, grp->slot_shift);
		if (roundedS == grp->S)
			return false;

		qfq_front_slot_remove(grp);
		qfq_slot_insert(q, grp, cl, roundedS);
	}

	return true;
}

/* Update system time V */
static void qfq_update_system_time(struct qfq_sched *q)
{
	u64 now;
	u64 t_diff;
	u64 v_diff = 0;
	u64 old_V;

	old_V = q->V;
	now = ktime_get().tv64;
	if (q->v_last_updated == now)
		return;

	t_diff = now - q->v_last_updated;

	/*
	 * Increment V to account for transmission time of earlier dequeued
	 * packets if required. Otherwise, just increment V based on the drain
	 * rate of the link.
	 */
	if (q->t_diff_sum) {
		if (t_diff >= q->t_diff_sum) {
			v_diff = q->v_diff_sum;
			t_diff -= q->t_diff_sum;
			q->v_diff_sum = 0;
			q->t_diff_sum = 0;
			/* After accounting for all previously dequeued packets,
			 * increment V at drain rate for remaining t_diff.
			 * Only do this if there aren't any eligible and ready
			 * groups currently. */
			if (!q->bitmaps[ER])
				v_diff += (u64)QFQ_DRAIN_RATE * t_diff / max((u32)LINK_SPEED, q->wsum_active);
		} else {
			v_diff = q->v_diff_sum * t_diff / q->t_diff_sum;
			q->v_diff_sum -= v_diff;
			q->t_diff_sum -= t_diff;
		}
	} else if (!q->bitmaps[ER]) {
		/* Increment V at line rate if no group is eligible and ready */
		v_diff = (u64)QFQ_DRAIN_RATE * t_diff / max((u32)LINK_SPEED, q->wsum_active);
	}

	q->V += v_diff;
	q->v_last_updated = now;

	/* Update group eligibility */
	qfq_update_eligible(q, old_V);
}

static struct sk_buff *qfq_dummy_dequeue(struct Qdisc *sch)
{
	qdisc_throttled(sch);
	return NULL;
}

static struct sk_buff *qfq_dequeue(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_group *grp;
	struct qfq_class *cl;
	struct sk_buff *skb;
	unsigned int len;
	unsigned int next_len = 0;
	int cl_qlen;
	spinlock_t *class_lock;
	u64 old_V;

	/* Update system time V */
	qfq_update_system_time(q);
	if (!q->bitmaps[ER])
		return NULL;

	grp = qfq_ffs(q, q->bitmaps[ER]);

	cl = qfq_slot_head(grp);
	class_lock = qdisc_lock(cl->qdisc);
	spin_lock(class_lock);
	skb = qdisc_dequeue_peeked(cl->qdisc);
	cl_qlen = qdisc_qlen(cl->qdisc);
	if (skb && cl_qlen) {
//		s64 now = ktime_get().tv64;
//		s64 dt = now - cl->prev_dequeue_time_ns;
//		s64 dev = dt - cl->expected_inter_dequeue_time_ns;
//		if (dev < 0) dev = -dev;
//		cl->prev_dequeue_time_ns = now;
//		/* Calculate EWMA */
//		cl->inter_dequeue_time_ns = ((cl->inter_dequeue_time_ns * 7) + dt) >> 3;
//		cl->absdev_dequeue_time_ns = ((cl->absdev_dequeue_time_ns * 7) + dev) >> 3;
		next_len = qdisc_peek_len(cl->qdisc);
	}
	spin_unlock(class_lock);

	if (!skb) {
		WARN_ONCE(1, "qfq_dequeue: non-workconserving leaf\n");
		return NULL;
	}

	/* sch->q.qlen for the QFQ-RL qdisc now denotes the number of activated
	 * classes. This value is only updated in the dequeue thread.
	 */
	if (!cl_qlen)
		sch->q.qlen--;

	qdisc_bstats_update(sch, skb);

	old_V = q->V;
	len = qdisc_pkt_len(skb);
	//q->V += (u64)len * ONE_FP / max((u32)LINK_SPEED, q->wsum_active);
	/*
	 * System time V will be updated over time (real time) rather than
	 * instantaneously. We just increment appropriate counters now.
	 */
	q->v_diff_sum += (u64)len * ONE_FP / max((u32)LINK_SPEED, q->wsum_active);
	q->t_diff_sum += (u64)len * NSEC_PER_SEC / (125000 * LINK_SPEED);
	pr_debug("qfq dequeue: len %u F %lld now %lld\n",
		 len, (unsigned long long) cl->F, (unsigned long long) q->V);

	if (qfq_update_class(q, grp, cl, next_len)) {
		u64 old_F = grp->F;

		//q->update_grp_on_deq++;
		if (cl->inv_w && !cl_qlen)
			q->wsum_active -= ONE_FP / cl->inv_w;

		cl = qfq_slot_scan(grp);
		if (!cl)
			__clear_bit(grp->index, &q->bitmaps[ER]);
		else {
			u64 roundedS = qfq_round_down(cl->S, grp->slot_shift);
			unsigned int s;

			if (grp->S == roundedS)
				goto skip_unblock;
			grp->S = roundedS;
			grp->F = roundedS + (2ULL << grp->slot_shift);
			__clear_bit(grp->index, &q->bitmaps[ER]);
			s = qfq_calc_state(q, grp);
			__set_bit(grp->index, &q->bitmaps[s]);
		}

		qfq_unblock_groups(q, grp->index, old_F);
	} else if (cl->inv_w && !cl_qlen)
		q->wsum_active -= ONE_FP / cl->inv_w;

skip_unblock:
	qfq_update_eligible(q, old_V);
//	if (!qdisc_qlen(sch))
//		q->idle_on_deq++;

	return skb;
}

/*
 * Assign a reasonable start time for a new flow k in group i.
 * Admissible values for \hat(F) are multiples of \sigma_i
 * no greater than V+\sigma_i . Larger values mean that
 * we had a wraparound so we consider the timestamp to be stale.
 *
 * If F is not stale and F >= V then we set S = F.
 * Otherwise we should assign S = V, but this may violate
 * the ordering in ER. So, if we have groups in ER, set S to
 * the F_j of the first group j which would be blocking us.
 * We are guaranteed not to move S backward because
 * otherwise our group i would still be blocked.
 */
static void qfq_update_start(struct qfq_sched *q, struct qfq_class *cl)
{
	unsigned long mask;
	u64 limit, roundedF;
	int slot_shift = cl->grp->slot_shift;

	roundedF = qfq_round_down(cl->F, slot_shift);
	limit = qfq_round_down(q->V, slot_shift) + (1ULL << slot_shift);

	if (!qfq_gt(cl->F, q->V) || qfq_gt(roundedF, limit)) {
		/* timestamp was stale */
		mask = mask_from(q->bitmaps[ER], cl->grp->index);
		if (mask) {
			struct qfq_group *next = qfq_ffs(q, mask);
			if (qfq_gt(roundedF, next->F)) {
				if (qfq_gt(limit, next->F))
					cl->S = next->F;
				else /* preserve timestamp correctness */
					cl->S = limit;
				return;
			}
		}
		cl->S = q->V;
	} else  /* timestamp is not stale */
		cl->S = cl->F;
}

static void qfq_enqueue_work_entry(struct qfq_sched *q, struct qfq_class *cl,
				   unsigned int pkt_len)
{
	struct qfq_cpu_work_queue *work_queue = this_cpu_ptr(q->work_queue);
	struct qfq_cpu_work_entry *ent = kzalloc(sizeof(*ent), GFP_ATOMIC);
	unsigned int cpu;
	if (ent == NULL) {
		printk("qfq_enqueue: FIX FIX FIX -- Work entry creation failed."
		       " Class not activated. This can cause problems.\n");
		return;
	}

	ent->cl = cl;
	ent->pkt_len = pkt_len;
	smp_mb();

	spin_lock(&work_queue->lock);
	list_add_tail(&ent->list, &work_queue->list);
	spin_unlock(&work_queue->lock);

	cpu = smp_processor_id();
	set_bit(cpu, &q->work_bitmap);
}

static int qfq_enqueue(struct sk_buff *skb, struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl;
	spinlock_t *class_lock;
	int cl_qlen = 0;
	int err = 0;
	cl = qfq_classify(skb, sch, &err);
	if (cl == NULL) {
		if (err & __NET_XMIT_BYPASS)
			sch->qstats.drops++;
		kfree_skb(skb);
		return err;
	}

	if (skb->sk) {
		skb->sk->qdisc_cache = (void *)sch;
		skb->sk->cl_cache = (void *)cl;
	}

	pr_debug("qfq_enqueue: cl = %x\n", cl->common.classid);

	class_lock = qdisc_lock(cl->qdisc);
	spin_lock(class_lock);
	err = qdisc_enqueue(skb, cl->qdisc);
	cl_qlen = qdisc_qlen(cl->qdisc);
	spin_unlock(class_lock);

	if (unlikely(err != NET_XMIT_SUCCESS)) {
		pr_debug("qfq_enqueue: enqueue failed %d\n", err);
		if (net_xmit_drop_count(err)) {
			/* FIXME(siva): Class stats should be updated while
			 * holding the class lock. The QFQ qdisc needs other
			 * techniques for synchronization.
			 */
			cl->qstats.drops++;
			sch->qstats.drops++;
		}
		return err;
	}

	/* FIXME(siva): bstats are being updated without the class lock. */
	bstats_update(&cl->bstats, skb);
	//++sch->q.qlen;

	/* If the new skb is not the head of queue, then done here. */
	if (cl_qlen != 1)
		return err;

	/* If reach this point, queue q was idle */
	if (cl->inv_w != ONE_FP + 1) {
		qfq_enqueue_work_entry(q, cl, qdisc_pkt_len(skb));
		//qfq_activate_class(q, cl, qdisc_pkt_len(skb));
		//q->wsum_active += ONE_FP / cl->inv_w;
	}

	return err;
}

static int qfq_enqueue_safe(struct sk_buff *skb, struct Qdisc *sch)
{
	int rc;
	spinlock_t *root_lock = qdisc_lock(sch);

	spin_lock(root_lock);
	rc = qfq_enqueue(skb, sch);
	spin_unlock(root_lock);

	return rc;
}

/*
 * Handle class switch from idle to backlogged.
 */
static void qfq_activate_class(struct qfq_sched *q, struct qfq_class *cl,
			       unsigned int pkt_len)
{
	struct qfq_group *grp = cl->grp;
	u64 roundedS;
	int s;

	qfq_update_start(q, cl);

	/* compute new finish time and rounded start. */
	cl->F = cl->S + (u64)pkt_len * cl->inv_w;
	roundedS = qfq_round_down(cl->S, grp->slot_shift);

	/*
	 * insert cl in the correct bucket.
	 * If cl->S >= grp->S we don't need to adjust the
	 * bucket list and simply go to the insertion phase.
	 * Otherwise grp->S is decreasing, we must make room
	 * in the bucket list, and also recompute the group state.
	 * Finally, if there were no flows in this group and nobody
	 * was in ER make sure to adjust V.
	 */
	if (grp->full_slots) {
		if (!qfq_gt(grp->S, cl->S))
			goto skip_update;

		/* create a slot for this cl->S */
		qfq_slot_rotate(grp, roundedS);
		/* group was surely ineligible, remove */
		__clear_bit(grp->index, &q->bitmaps[IR]);
		__clear_bit(grp->index, &q->bitmaps[IB]);
	}
	/*
	 * For standard QFQ, if the group was empty before (all slots empty) and
	 * no other classes were [ER] then V would be lagging behind and must
	 * be updated to make this group eligible immediately. This occurs when
	 * the link was earlier idle and a new class needs to be activated.
	 */

	grp->S = roundedS;
	grp->F = roundedS + (2ULL << grp->slot_shift);
	s = qfq_calc_state(q, grp);
	__set_bit(grp->index, &q->bitmaps[s]);

	pr_debug("qfq enqueue: new state %d %#lx S %lld F %lld V %lld\n",
		 s, q->bitmaps[s],
		 (unsigned long long) cl->S,
		 (unsigned long long) cl->F,
		 (unsigned long long) q->V);

skip_update:
	qfq_slot_insert(q, grp, cl, roundedS);
}


static void qfq_slot_remove(struct qfq_sched *q, struct qfq_group *grp,
			    struct qfq_class *cl)
{
	unsigned int i, offset;
	u64 roundedS;

	roundedS = qfq_round_down(cl->S, grp->slot_shift);
	offset = (roundedS - grp->S) >> grp->slot_shift;
	i = (grp->front + offset) % QFQ_MAX_SLOTS;

	hlist_del(&cl->next);
	if (hlist_empty(&grp->slots[i]))
		__clear_bit(offset, &grp->full_slots);
}

/*
 * called to forcibly destroy a queue.
 * If the queue is not in the front bucket, or if it has
 * other queues in the front bucket, we can simply remove
 * the queue with no other side effects.
 * Otherwise we must propagate the event up.
 */
static void qfq_deactivate_class(struct qfq_sched *q, struct qfq_class *cl)
{
	struct qfq_group *grp = cl->grp;
	unsigned long mask;
	u64 roundedS;
	int s;

	cl->F = cl->S;
	qfq_slot_remove(q, grp, cl);

	if (!grp->full_slots) {
		__clear_bit(grp->index, &q->bitmaps[IR]);
		__clear_bit(grp->index, &q->bitmaps[EB]);
		__clear_bit(grp->index, &q->bitmaps[IB]);

		if (test_bit(grp->index, &q->bitmaps[ER]) &&
		    !(q->bitmaps[ER] & ~((1UL << grp->index) - 1))) {
			mask = q->bitmaps[ER] & ((1UL << grp->index) - 1);
			if (mask)
				mask = ~((1UL << __fls(mask)) - 1);
			else
				mask = ~0UL;
			qfq_move_groups(q, mask, EB, ER);
			qfq_move_groups(q, mask, IB, IR);
		}
		__clear_bit(grp->index, &q->bitmaps[ER]);
	} else if (hlist_empty(&grp->slots[grp->front])) {
		cl = qfq_slot_scan(grp);
		roundedS = qfq_round_down(cl->S, grp->slot_shift);
		if (grp->S != roundedS) {
			__clear_bit(grp->index, &q->bitmaps[ER]);
			__clear_bit(grp->index, &q->bitmaps[IR]);
			__clear_bit(grp->index, &q->bitmaps[EB]);
			__clear_bit(grp->index, &q->bitmaps[IB]);
			grp->S = roundedS;
			grp->F = roundedS + (2ULL << grp->slot_shift);
			s = qfq_calc_state(q, grp);
			__set_bit(grp->index, &q->bitmaps[s]);
		}
	}

	qfq_update_eligible(q, q->V);
}

static void qfq_qlen_notify(struct Qdisc *sch, unsigned long arg)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl = (struct qfq_class *)arg;

	if (cl->qdisc->q.qlen == 0)
		qfq_deactivate_class(q, cl);
}

static unsigned int qfq_drop(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_group *grp;
	unsigned int i, j, len;

	for (i = 0; i <= QFQ_MAX_INDEX; i++) {
		grp = &q->groups[i];
		for (j = 0; j < QFQ_MAX_SLOTS; j++) {
			struct qfq_class *cl;

			hlist_for_each_entry(cl, &grp->slots[j], next) {

				if (!cl->qdisc->ops->drop)
					continue;

				len = cl->qdisc->ops->drop(cl->qdisc);
				if (len > 0) {
					sch->q.qlen--;
					if (!cl->qdisc->q.qlen)
						qfq_deactivate_class(q, cl);

					return len;
				}
			}
		}
	}

	return 0;
}

static int qfq_dump_qdisc_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct tc_qfq_xstats xstats = {.type = TCA_QFQ_XSTATS_QDISC};

//	xstats.qdisc_stats.v_forwarded = q->v_forwarded;
//	xstats.qdisc_stats.idle_on_deq = q->idle_on_deq;
//	xstats.qdisc_stats.update_grp_on_deq = q->update_grp_on_deq;
//	xstats.qdisc_stats.txq_blocked = q->txq_blocked;
	xstats.qdisc_stats.wsum_active = q->wsum_active;

	return gnet_stats_copy_app(d, &xstats, sizeof(xstats));
}

/*
 * Wait until a packet is enqueued in the qdisc.
 * We call the kernel schedule() function and check if the kthread should stop
 * only once every few iterations of the queue length checking loop if the
 * qdisc is idle.
 */
static void qfq_spinner_wait_for_skb(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	int schedule_counter = 0;
	while ((qdisc_qlen(sch) == 0) &&
	       (schedule_counter || !kthread_should_stop()) &&
	       (!q->work_bitmap)) {
		schedule_counter++;
		if (schedule_counter >= 10000) {
			schedule_counter = 0;
			schedule();
		}
	}
}

static void qfq_spinner_activate_classes(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	unsigned int cpu;

	/* We just check the work bitmap without atomicity to see if there is
	 * any  work at all. Even if it is incorrect, we would eventually read
	 * the correct values in another iteration.
	 */
	if (!q->work_bitmap)
		return;

	qfq_update_system_time(qdisc_priv(sch));
	for_each_possible_cpu(cpu) {
		struct qfq_cpu_work_queue *work_queue;
		struct qfq_cpu_work_entry *ent, *tmp_ent;

		if (!test_and_clear_bit(cpu, &q->work_bitmap))
			continue;

		/* Process all class activation requests for the CPU */
		work_queue = per_cpu_ptr(q->work_queue, cpu);
		spin_lock(&work_queue->lock);
		list_for_each_entry_safe(ent, tmp_ent, &work_queue->list, list) {
			list_del(&ent->list);
			/* We do not acquire the class lock here since we only activate the
			 * class and do not update the class qdisc.
			 */
			qfq_activate_class(q, ent->cl, ent->pkt_len);
			q->wsum_active += ONE_FP / ent->cl->inv_w;
			++sch->q.qlen;
			kfree(ent);
		}
		spin_unlock(&work_queue->lock);
	}
}

static int qfq_spinner(void *_qdisc)
{
	struct Qdisc *sch = _qdisc;
	struct qfq_sched *q = qdisc_priv(sch);
	struct task_struct *tsk = current;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct sk_buff *skb = NULL;
	struct net_device *dev;
	struct netdev_queue *txq;
	unsigned int skb_len;
	int rc;
	//int new_skb_deq = 0;
	int schedule_counter = 0;
	int queue_index;

	sched_setscheduler(tsk, SCHED_FIFO, &param);
	printk(KERN_INFO "Kernel thread qfq-spinner on cpu %d args %p q %p\n", smp_processor_id(), sch, q);

	while (!kthread_should_stop()) {
		/* Wait for a packet to be queued*/
		if (!skb)
			qfq_spinner_wait_for_skb(sch);

		/* Perform work items enqueued by CPUs */
		qfq_spinner_activate_classes(sch);

		if (likely(!skb)) {
			/* Call the real dequeue function */
			skb = qfq_dequeue(sch);

			if (unlikely(!skb))
				goto done;

			//new_skb_deq = 1;
		}

		dev = qdisc_dev(sch);

		/* Hash the skb on to one of the available queues */
		queue_index = skb_tx_hash(dev, skb);
		skb_set_queue_mapping(skb, queue_index);
		txq = netdev_get_tx_queue(dev, queue_index);

		/* The kernel does a lot of stuff which we can quickly
		 * bypass.  We know the features of our NIC --
		 * supports hardware checksumming, supports GSO, etc.
		 * And, only one thread dequeues packets.  So we don't
		 * need any lock.
		 */
		if (!netif_xmit_frozen_or_stopped(txq)) {
			skb_len = skb->len;
			rc = dev->netdev_ops->ndo_start_xmit(skb, dev);
			/* We should trace ndo_start_xmit similar to the way it
			 * is used in other places, but compiler complains that
			 * symbol was not found.
			 */
			/* trace_net_dev_xmit(skb, rc, dev, skb_len); */
			if (rc == NETDEV_TX_OK) {
				txq_trans_update(txq);
				skb = NULL;
			} /* else if (new_skb_deq) {
				new_skb_deq = 0;
				q->txq_blocked++;
			} */
		} /* else if (new_skb_deq) {
			new_skb_deq = 0;
			q->txq_blocked++;
		} */
done:

		/* Even when there are packets in the queue, we call the
		 * scheduler occasionally to avoid RCU stalls.
		 */
		schedule_counter++;
		if (schedule_counter >= 100000) {
			schedule_counter = 0;
			schedule();
		}
	}

	printk(KERN_INFO "Kernel thread qfq-spinner stopped on cpu %d\n", smp_processor_id());
	return 0;
}

static int qfq_init_qdisc(struct Qdisc *sch, struct nlattr *opt)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_group *grp;
	int i, j, err;
	unsigned int cpu;

	err = qdisc_class_hash_init(&q->clhash);
	if (err < 0)
		return err;

	for (i = 0; i <= QFQ_MAX_INDEX; i++) {
		grp = &q->groups[i];
		grp->index = i;
		grp->slot_shift = QFQ_MTU_SHIFT + FRAC_BITS
				   - (QFQ_MAX_INDEX - i);
		for (j = 0; j < QFQ_MAX_SLOTS; j++)
			INIT_HLIST_HEAD(&grp->slots[j]);
	}

//	q->v_forwarded = 0;
//	q->idle_on_deq = 0;
//	q->update_grp_on_deq = 0;
//	q->txq_blocked = 0;
	q->v_diff_sum = 0;
	q->t_diff_sum = 0;

	/* Allocate and initialize per CPU work queues */
	q->work_queue = alloc_percpu(struct qfq_cpu_work_queue);
	q->work_bitmap = 0;
	for_each_possible_cpu(cpu) {
		struct qfq_cpu_work_queue *work_queue = per_cpu_ptr(q->work_queue, cpu);
		INIT_LIST_HEAD(&work_queue->list);
		spin_lock_init(&work_queue->lock);
	}

	sch->flags |= TCQ_F_QFQ_RL;

	printk(KERN_INFO "Creating spinner args %p q %p\n", sch, q);
	q->spinner = kthread_create(qfq_spinner, (void *)sch, "qfq-spinner");

	/* In case the thread goes away ... */
	if (!IS_ERR(q->spinner)) {
		smp_mb();
		kthread_bind(q->spinner, spin_cpu);
		wake_up_process(q->spinner);
	}

	return 0;
}

static void qfq_reset_qdisc(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_group *grp;
	struct qfq_class *cl;
	struct hlist_node *tmp;
	unsigned int i, j;
	unsigned int cpu;

	for (i = 0; i <= QFQ_MAX_INDEX; i++) {
		grp = &q->groups[i];
		for (j = 0; j < QFQ_MAX_SLOTS; j++) {
			hlist_for_each_entry_safe(cl, tmp,
						  &grp->slots[j], next) {
				qfq_deactivate_class(q, cl);
			}
		}
	}

	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry(cl, &q->clhash.hash[i], common.hnode)
			qdisc_reset(cl->qdisc);
	}
	sch->q.qlen = 0;

	for_each_possible_cpu(cpu) {
		struct qfq_cpu_work_queue *work_queue;
		struct qfq_cpu_work_entry *ent, *tmp_ent;

		work_queue = per_cpu_ptr(q->work_queue, cpu);
		spin_lock(&work_queue->lock);
		list_for_each_entry_safe(ent, tmp_ent, &work_queue->list, list) {
			list_del(&ent->list);
			kfree(ent);
		}
		spin_unlock(&work_queue->lock);
	}
	q->work_bitmap = 0;
}

static void qfq_destroy_qdisc(struct Qdisc *sch)
{
	struct qfq_sched *q = qdisc_priv(sch);
	struct qfq_class *cl;
	struct hlist_node *next;
	unsigned int i;
	unsigned int cpu;

	printk(KERN_INFO "waiting for thread %p to stop\n", q->spinner);
	if (!IS_ERR(q->spinner)) {
		kthread_stop(q->spinner);
	}

	tcf_destroy_chain(&q->filter_list);

	for (i = 0; i < q->clhash.hashsize; i++) {
		hlist_for_each_entry_safe(cl, next, &q->clhash.hash[i],
					  common.hnode) {
			qfq_destroy_class(sch, cl);
		}
	}
	qdisc_class_hash_destroy(&q->clhash);

	/* Free the CPU work queues */
	for_each_possible_cpu(cpu) {
		struct qfq_cpu_work_queue *work_queue;
		struct qfq_cpu_work_entry *ent, *tmp_ent;

		work_queue = per_cpu_ptr(q->work_queue, cpu);
		spin_lock(&work_queue->lock);
		list_for_each_entry_safe(ent, tmp_ent, &work_queue->list, list) {
			list_del(&ent->list);
			kfree(ent);
		}
		spin_unlock(&work_queue->lock);
	}
	free_percpu(q->work_queue);
	q->work_queue = NULL;
	q->work_bitmap = 0;
}

static const struct Qdisc_class_ops qfq_class_ops = {
	.change		= qfq_change_class,
	.delete		= qfq_delete_class,
	.get		= qfq_get_class,
	.put		= qfq_put_class,
	.tcf_chain	= qfq_tcf_chain,
	.bind_tcf	= qfq_bind_tcf,
	.unbind_tcf	= qfq_unbind_tcf,
	.graft		= qfq_graft_class,
	.leaf		= qfq_class_leaf,
	.qlen_notify	= qfq_qlen_notify,
	.dump		= qfq_dump_class,
	.dump_stats	= qfq_dump_class_stats,
	.walk		= qfq_walk,
};

static struct Qdisc_ops qfq_qdisc_ops __read_mostly = {
	.cl_ops		= &qfq_class_ops,
	.id		= "qfq",
	.priv_size	= sizeof(struct qfq_sched),
	.enqueue	= qfq_enqueue,
	/*.dequeue	= qfq_dequeue, */
	.dequeue        = qfq_dummy_dequeue,
	.peek		= qdisc_peek_dequeued,
	.drop		= qfq_drop,
	.init		= qfq_init_qdisc,
	.reset		= qfq_reset_qdisc,
	.destroy	= qfq_destroy_qdisc,
	.owner		= THIS_MODULE,
	.dump_stats	= qfq_dump_qdisc_stats,
};

static int __init qfq_init(void)
{
	return register_qdisc(&qfq_qdisc_ops);
}

static void __exit qfq_exit(void)
{
	unregister_qdisc(&qfq_qdisc_ops);
}

module_init(qfq_init);
module_exit(qfq_exit);
MODULE_LICENSE("GPL");
