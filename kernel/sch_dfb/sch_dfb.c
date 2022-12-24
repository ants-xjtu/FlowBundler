/*
 * DFB: Different flows in one burst
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/bitops.h>
#include <net/netlink.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>

#define DFB_DEBUG 0
#define DFB_PROFILE 0

// scaling factor of bucket number
#define BUCKET_NUM_SCALE 10
#define BUCKET_NUM (1 << BUCKET_NUM_SCALE)
#define QUEUE_NUM BITS_PER_LONG
#define MTU 1500
// scaling factor of minimum granularity (in ns)
#define MIN_GRAN_SCALE 0
#define MILLION 1000000

// This mask is used to round bucket index: bucket_id = bucket_id & bid_msak
static u32 bid_mask = (1 << BUCKET_NUM_SCALE) - 1;
// Maximum supported pacing rate, in byte per second
static u64 max_rate = NSEC_PER_SEC * MTU >> MIN_GRAN_SCALE;
static u32 max_rate_highest_bit = 0;

struct dfb_bucket {
    struct sk_buff *head;
    struct sk_buff *tail;
};

struct dfb_location {
    u8 queue_id;
    u64 bucket_id;
};

struct dfb_sched_data {
    // multi-level calendar queue
    struct dfb_bucket mlcq[QUEUE_NUM][BUCKET_NUM];
    u32 n_packets_total;
    // # of packets in each queue
    u32 n_packets[QUEUE_NUM];
    // each bit indicates whether a queue is empty
    u64 bitmap;
    u64 head_time; // head time in ns
    // the queue id with the highest pacing rate among all active flows
    u8 highest_rate_qid;
    u8 real_qid_cache[QUEUE_NUM][BUCKET_NUM];
#if DFB_PROFILE
    u64 enqueue_cycles;
    u64 dequeue_cycles;
#endif
    struct qdisc_watchdog watchdog;
};

unsigned int queue_limit __read_mostly = 100000;
MODULE_PARM_DESC(queue_limit, "Maximum queue size in packets");
module_param(queue_limit, uint, 0644);

static bool gso_split __read_mostly = false;
MODULE_PARM_DESC(gso_split, "Whether to split GSO packets before enqueue.");
module_param(gso_split, bool, 0644);

#define dfb_bucket_is_empty(bucket) ((bucket)->head == NULL)

/*
 * Note that the "+1".
 * Without "+1", the packet may be put in to a bucket
 * whose expected send time is smaller than the head_time.
 * As a result, the bucket will be visited in the next round.
 * This is very long time if the pacing rate is very low
 */
#define dfb_find_bucket_index(queue_id, send_time) \
        (((send_time) >> ((queue_id) + MIN_GRAN_SCALE)) + 1)

#define dfb_get_bucket(q, location) \
        (&((q)->mlcq[(location).queue_id][(location).bucket_id & bid_mask]))

#define dfb_head_time_mask(queue_id) \
        ((~((u64) 0)) << (queue_id))

static inline void dfb_get_flow_info(struct sk_buff *skb, char *buff, size_t len)
{
    struct iphdr *iph = NULL;
    if (unlikely(skb == NULL)) {
        return ;
    }
    /* Extract headers */
    iph = ip_hdr(skb);
    if(iph && iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcph = tcp_hdr(skb);
        scnprintf(
            buff, len,
            "%pI4:%u->%pI4:%u,seq=%u",
            &(iph->saddr), ntohs(tcph->source),
            &(iph->daddr), ntohs(tcph->dest),
            ntohl(tcph->seq)
        );
    }
}

static inline u64* dfb_get_prev_ts(struct sock *sk) {
    return &(tcp_sk(sk)->pacing_timer._softexpires);
}
/*
 * Push a packet into the bucket
 */
static inline void dfb_push_to_bucket(struct dfb_sched_data *q,
        struct dfb_location location, struct sk_buff *skb)
{
    struct dfb_bucket *bkt = dfb_get_bucket(q, location);
    skb->next = NULL;
    if (dfb_bucket_is_empty(bkt)) {
        bkt->head = bkt->tail = skb;
    } else {
        bkt->tail->next = skb;
        bkt->tail = skb;
    }
    q->n_packets[location.queue_id] ++;
    q->n_packets_total ++;
    q->bitmap |= (1L<<location.queue_id);
#if DFB_DEBUG >= 2
        pr_info("push,n_pkts[%u]=%u, n_pkts=%u, bitmap=0x%llx\n", location.queue_id, q->n_packets[location.queue_id], q->n_packets_total, q->bitmap);
#endif
}

/*
 * Pop a packet from the bucket
 */
static inline struct sk_buff *dfb_pop_from_bucket(struct dfb_sched_data *q,
        struct dfb_location location)
{
    struct dfb_bucket *bkt = dfb_get_bucket(q, location);
    struct sk_buff *skb = bkt->head;
    if (skb) {
        bkt->head = skb->next;
        skb->next = NULL;
        q->n_packets[location.queue_id] --;
        if (q->n_packets[location.queue_id] == 0) {
            q->bitmap &= ~(1L<<location.queue_id);
        }
        q->n_packets_total --;
#if DFB_DEBUG >= 2
        pr_info("pop,n_pkts[%u]=%u, n_pkts=%u, bitmap=0x%llx\n", location.queue_id, q->n_packets[location.queue_id], q->n_packets_total, q->bitmap);
#endif
    }
    return skb;
}

/*
 * Pop all packets from the bucket
 */
static inline struct sk_buff *dfb_pop_bucket(struct dfb_sched_data *q,
        struct dfb_location location)
{
    struct dfb_bucket *bkt = dfb_get_bucket(q, location);
    struct sk_buff *head = bkt->head;
    struct sk_buff *skb = head;
    while (skb) {
        q->n_packets[location.queue_id] --;
        q->n_packets_total --;
#if DFB_DEBUG >= 2
        pr_info("pop,n_pkts[%u]=%u, n_pkts=%u, bitmap=0x%llx\n", location.queue_id, q->n_packets[location.queue_id], q->n_packets_total, q->bitmap);
#endif
        skb = skb->next;
    }
    bkt->head = NULL;
    if (q->n_packets[location.queue_id] == 0) {
        q->bitmap &= ~(1L<<location.queue_id);
    }
    return head;
}

static inline u8 dfb_find_queue_index(u64 rate)
{
    u8 id = 0;
    rate = min_t(u64, max_rate, rate);
    id = max_rate_highest_bit - fls64(rate);
#if DFB_DEBUG >= 2
    pr_info("%llu.%06lluMBps qid=%u\n", rate/MILLION, rate%MILLION, id);
#endif
    return id;
}

static inline struct dfb_location dfb_find_real_location(
    struct dfb_sched_data *q, u8 queue_id, u64 send_time)
{
    struct dfb_location real_loc;
    u64 bid = dfb_find_bucket_index(queue_id, send_time);
#if DFB_DEBUG >= 2
    pr_info(
        "find_real: qid=%u, send_time=%llu.%06llums\n",
        queue_id, send_time / NSEC_PER_MSEC, send_time % NSEC_PER_MSEC
    );
#endif
    // real_loc.queue_id = queue_id + q->real_qid_cache[queue_id][bid];
#if DFB_DEBUG >= 2
    pr_info("find_real: bid=%llu\n", bid);
#endif
    real_loc.queue_id = queue_id + __ffs(bid | 1L<<(QUEUE_NUM-1)); // TODO: this can be optimized
    if(unlikely(real_loc.queue_id) >= QUEUE_NUM) {
        real_loc.queue_id = QUEUE_NUM - 1;
    }
#if DFB_DEBUG >= 2
    pr_info("find_real: real_qid=%u\n", real_loc.queue_id);
#endif
    real_loc.bucket_id = dfb_find_bucket_index(real_loc.queue_id, send_time);
#if DFB_DEBUG >= 2
    pr_info("find_real: real_bid=%llu\n", real_loc.bucket_id);
#endif
    return real_loc;
}

static void dfb_enqueue_to_cq(struct dfb_sched_data *q, struct sk_buff *skb,
                              u8 queue_id)
{
    u64 head_time = q->head_time;
    // maximum time stamp of a calendar queue
    u64 granularity = 1L << (queue_id + MIN_GRAN_SCALE);
    u64 horizon = (granularity << BUCKET_NUM_SCALE) - granularity;
    u64 send_time = skb->skb_mstamp_ns;
#if DFB_DEBUG
    u64 now = 0;
    u64 bid = 0;
    char buff[100];
#endif
    // real queue index and bucket index
    struct dfb_location real_loc = {
        .queue_id = 0,
        .bucket_id = 0,
    };
    /* First packet */
    if (q->bitmap == 0) {
        q->head_time = send_time & dfb_head_time_mask(queue_id);
        q->highest_rate_qid = queue_id;
    }
    head_time = q->head_time;
    send_time = clamp_t(u64, send_time, head_time, head_time + horizon);
    real_loc = dfb_find_real_location(q, queue_id, send_time);
#if DFB_DEBUG >= 1
    now = ktime_get_ns();
    bid = dfb_find_bucket_index(queue_id, send_time);
    dfb_get_flow_info(skb, buff, 100);
    pr_info(
        "e, %llu.%06llums, head=%llu.%06llums, send=%llu.%06llums, %uB, %u, %llu(%llu), %u, %llu(%llu), %s\n",
        now / NSEC_PER_MSEC, now % NSEC_PER_MSEC,
        q->head_time / NSEC_PER_MSEC, q->head_time % NSEC_PER_MSEC,
        send_time / NSEC_PER_MSEC, send_time % NSEC_PER_MSEC,
        skb->len, queue_id, bid, bid & bid_mask,
        real_loc.queue_id, real_loc.bucket_id, real_loc.bucket_id & bid_mask,
        buff
    );
#endif
    dfb_push_to_bucket(q, real_loc, skb);
}

/*
 * Segment gso packets
 */
static int dfb_segment(struct sk_buff *skb, struct Qdisc *sch,
                       struct sk_buff **to_free)
{
    struct sock *sk = skb->sk;
    struct tcp_sock *tp = NULL;
    unsigned long rate = (sk != NULL ? sk->sk_pacing_rate : 0);
    struct dfb_sched_data *q = qdisc_priv(sch);
    struct sk_buff *segs = NULL, *nskb = NULL;
    netdev_features_t features = netif_skb_features(skb);
    unsigned int len = 0;
    int nb = 0;
    u8 qid = 0;
    u64 ntime = skb->skb_mstamp_ns;
    u64 send_time = ntime;
    u64 payload_len = 0;
    u8 hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
    // u64 *prev_ts = dfb_get_prev_ts(sk);

    if (unlikely(rate == 0)) {
        return qdisc_enqueue_tail(skb, sch);
    }
    tp = tcp_sk(sk);
    /* Speed up!
     * In this case, the packet process overhead is so high that
     * the expected transmission time of a packet lags behind the now time,
     * The next transmission time will be smaller than send_time + tso_len / rate.
     * (See `tcp_update_skb_after_send`: `len_ns -= min_t(u64, len_ns / 2, credit);`).
     * If we increase the transmission time of each packet by len/rate,
     * some packets may be transmitted after the next (TSO) packet.
     * Through experiment, the consequence out-of-order can result in
     * a 8x throughput degradation with pacing rate set to 8Gbps.
     * A direct approach is to remember the transmission time of the last packet.
     * However, it needs to maintain per-flow data.
     * The follow approach increases the pacing rate, which can increase the traffic burstiness.
     * Note that this temporary approach should be improved in the future.
     */
    /*if (tp->tcp_wstamp_ns <= tp->tcp_clock_cache) {
        rate <<= 1;
    }*/
    segs = skb_gso_segment(skb, features & ~NETIF_F_GSO_MASK);

    if (IS_ERR_OR_NULL(segs)) {
        return qdisc_drop(skb, sch, to_free);
    }

    qid = dfb_find_queue_index(rate);
    q->highest_rate_qid = min_t(u8, q->highest_rate_qid, qid);
    nb = 0;
    while (segs) {
        send_time = ntime;
        nskb = segs->next;
        skb_mark_not_on_list(segs);
        qdisc_skb_cb(segs)->pkt_len = segs->len;
        segs->skb_mstamp_ns = send_time;
        len += segs->len;
        dfb_enqueue_to_cq(q, segs, qid);
        nb++;
        payload_len = segs->len;
        payload_len = payload_len - min_t(u64, hdr_len, payload_len);
        ntime = send_time + div64_ul(payload_len * NSEC_PER_SEC, rate);
        segs = nskb;
    }
    // (*prev_ts) = send_time;
    consume_skb(skb);
    return nb > 0 ? NET_XMIT_SUCCESS : NET_XMIT_DROP;
}

static int dfb_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                  struct sk_buff **to_free)
{
#if DFB_PROFILE
    u64 n_tscs = rdtsc();
#endif
    struct dfb_sched_data *q = qdisc_priv(sch);
    int ret = 0;
    // flow pacing rate
    struct sock *sk = skb->sk;
    u64 rate = (sk ? sk->sk_pacing_rate : 0);
    // queue index
    u8 qid = 0;
    // u64 *prev_ts = dfb_get_prev_ts(sk);
#if DFB_DEBUG >= 2
    u64 now = ktime_get_ns();
#endif

    if (sk && smp_load_acquire(&sk->sk_pacing_status) != SK_PACING_FQ) {
        smp_store_release(&sk->sk_pacing_status, SK_PACING_FQ);
    }

    if (likely(q->n_packets_total < queue_limit)) {
        // The flow is not paced
        if (rate == 0) {
            ret = qdisc_enqueue_tail(skb, sch);
            goto dfb_enqueue_done;
        }
        // pr_info("%lluns, %lluns, %uB\n", ktime_get_ns(), skb->skb_mstamp_ns, skb->len);
#if DFB_DEBUG >= 2
        pr_info(
            "+, %llu.%06llums, send=%llu.%06llums, head=%llu.%06llums, %uB, %llu.%06lluMBps\n",
            now / NSEC_PER_MSEC, now % NSEC_PER_MSEC,
            skb->skb_mstamp_ns / NSEC_PER_MSEC, skb->skb_mstamp_ns % NSEC_PER_MSEC,
            q->head_time / NSEC_PER_MSEC, q->head_time % NSEC_PER_MSEC,
            skb->len, rate / MILLION, rate % MILLION
        );
#endif
        // skb->skb_mstamp_ns = max_t(u64, *prev_ts, skb->skb_mstamp_ns);
        if (gso_split && skb_is_gso(skb)) {
            ret = dfb_segment(skb, sch, to_free);
            goto dfb_enqueue_done;
        }
        qid = dfb_find_queue_index(rate);
        q->highest_rate_qid = min_t(u8, q->highest_rate_qid, qid);
        dfb_enqueue_to_cq(q, skb, qid);
        // *prev_ts = skb->skb_mstamp_ns;
#if DFB_DEBUG >= 2
        pr_info(
            "++, %up, 0x%llx\n", q->n_packets_total, q->bitmap
        );
#endif
        ret = NET_XMIT_SUCCESS;
        goto dfb_enqueue_done;
    }
    ret = qdisc_drop(skb, sch, to_free);
dfb_enqueue_done:
#if DFB_PROFILE
    n_tscs = rdtsc() - n_tscs;
    q->enqueue_cycles += n_tscs;
#endif
    return ret;
}

static struct sk_buff *dfb_dequeue(struct Qdisc *sch)
{
#if DFB_PROFILE
    u64 n_tscs = rdtsc();
#endif
#if DFB_DEBUG
    char buff[100];
    u64 bid = 0;
#endif
    struct sk_buff *skb = NULL;
    struct dfb_sched_data *q = qdisc_priv(sch);
    u8 highest_rate_qid = q->highest_rate_qid;
    /*
     * Note that the head time must be the smallest time of a bucket.
     * Otherwise, when the maximum queue index increases,
     * the current bucket can be skipped.
     */
    u64 head_time = (q->head_time) & dfb_head_time_mask(highest_rate_qid);
    u64 next_time = 0;
    u64 granularity = 0;
    u64 now = 0;
    struct dfb_location real_loc;

    skb = qdisc_dequeue_head(sch);
    if (skb) {
        goto dfb_dequeue_done;
    }
    if (q->bitmap == 0) {
        goto dfb_dequeue_done;
    }
#if DFB_DEBUG >= 1
    if (q->n_packets_total == 0) {
        pr_err("ERROR: no packets in queue while bitmap=%llu\n", q->bitmap);
        return NULL;
    }
#endif
    // granularity of the highest rate queue
    granularity = 1L << (highest_rate_qid + MIN_GRAN_SCALE);
    now = ktime_get_ns();
#if DFB_DEBUG >= 2
    bid = dfb_find_bucket_index(highest_rate_qid, head_time);
    pr_info(
        "-, %llu.%06llums, head=%llu.%06llums, gran=%lluns, %u, %llu(%llu)\n",
        now / NSEC_PER_MSEC, now % NSEC_PER_MSEC,
        head_time / NSEC_PER_MSEC, head_time % NSEC_PER_MSEC,
        granularity,
        highest_rate_qid, bid, bid & bid_mask
    );
#endif
    while (now >= head_time) {
        real_loc = dfb_find_real_location(q, highest_rate_qid, head_time);
#if DFB_DEBUG >= 2
        bid = dfb_find_bucket_index(highest_rate_qid, head_time);
        pr_info(
            "s, head=%llu.%06llums, %u, %llu(%llu), %u, %llu(%llu)\n",
            head_time / NSEC_PER_MSEC, head_time % NSEC_PER_MSEC,
            highest_rate_qid, bid, bid & bid_mask,
            real_loc.queue_id, real_loc.bucket_id,
            real_loc.bucket_id & bid_mask
        );
#endif
        skb = dfb_pop_from_bucket(q, real_loc);
        // skb = dfb_pop_bucket(q, real_loc);
        if (skb) {
            q->head_time = head_time;
            if (q->n_packets[highest_rate_qid] == 0) {
                q->highest_rate_qid = \
                    q->bitmap ? __ffs(q->bitmap) : QUEUE_NUM - 1;
            }
#if DFB_DEBUG >= 1
            bid = dfb_find_bucket_index(highest_rate_qid, head_time);
            dfb_get_flow_info(skb, buff, 100);
            pr_info(
                "d, %llu.%06llums, %uB, %u, %llu(%llu), %u, %llu(%llu), %up, 0x%llx, %s\n",
                now / NSEC_PER_MSEC, now % NSEC_PER_MSEC,
                skb->len,
                highest_rate_qid, bid, bid & bid_mask,
                real_loc.queue_id, real_loc.bucket_id,
                real_loc.bucket_id & bid_mask,
                q->n_packets_total, q->bitmap,
                buff
            );
#endif
            goto dfb_dequeue_done;
        } else {
            head_time += granularity;
        }
    }
    q->head_time = now & dfb_head_time_mask(highest_rate_qid);
    // find next packet
    next_time = head_time - granularity;
    do {
        next_time += granularity;
        real_loc = dfb_find_real_location(q, highest_rate_qid, next_time);
#if DFB_DEBUG >= 2
        bid = dfb_find_bucket_index(highest_rate_qid, next_time);
        pr_info(
            "n, next=%llu.%06llums, %u, %llu(%llu), %u, %llu(%llu)\n",
            next_time / NSEC_PER_MSEC, next_time % NSEC_PER_MSEC,
            highest_rate_qid, bid, bid & bid_mask,
            real_loc.queue_id, real_loc.bucket_id,
            real_loc.bucket_id & bid_mask
        );
#endif
    } while (dfb_bucket_is_empty(dfb_get_bucket(q, real_loc)));
#if DFB_DEBUG >= 2
    bid = dfb_find_bucket_index(highest_rate_qid, next_time);
    pr_info(
        "n, next=%llu.%06llums, %u, %llu(%llu), %u, %llu(%llu)\n",
        next_time / NSEC_PER_MSEC, next_time % NSEC_PER_MSEC,
        highest_rate_qid, bid, bid & bid_mask,
        real_loc.queue_id, real_loc.bucket_id,
        real_loc.bucket_id & bid_mask
    );
#endif
    qdisc_watchdog_schedule_ns(&q->watchdog, next_time);
dfb_dequeue_done:
#if DFB_PROFILE
    n_tscs = rdtsc() - n_tscs;
    q->dequeue_cycles += n_tscs;
#endif
    return skb;
}

static void dfb_destroy(struct Qdisc *sch)
{
    struct dfb_sched_data *q = qdisc_priv(sch);
    pr_info("Destroy qdisc %s\n", sch->ops->id);
#if DFB_PROFILE
    pr_info("enqueue_cycles=%llu, dequeue_cycles=%llu\n", q->enqueue_cycles, q->dequeue_cycles);
#endif
    qdisc_watchdog_cancel(&q->watchdog);
}

static inline void dfb_init_bucket(struct dfb_bucket *bucket)
{
    bucket->head = bucket->tail = NULL;
}

static int dfb_init(struct Qdisc *sch, struct nlattr *opt,
        struct netlink_ext_ack *extack)
{
    struct dfb_sched_data *q = qdisc_priv(sch);
    u8 qid = 0;
    u64 bid = 0;

    /*This allows bulk dequeue: https://lwn.net/Articles/615240/*/
    /*20220106: this may cause "Detected Tx Unit Han" error*/
    // sch->flags |= TCQ_F_ONETXQUEUE;
    sch->limit = queue_limit;
    pr_info("Init qdisc %s\n", sch->ops->id);
    pr_info("limit=%u, gso_split=%u\n", sch->limit, gso_split);

    q->bitmap = 0;
    q->n_packets_total = 0;
    q->head_time = ktime_get_ns();
    q->highest_rate_qid = QUEUE_NUM - 1;
#if DFB_PROFILE
    q->enqueue_cycles = 0;
    q->dequeue_cycles = 0;
#endif
    /*
     * Initialize calendar queue
     */
    for (qid = 0; qid < QUEUE_NUM; qid++) {
        q->n_packets[qid] = 0;
        for (bid = 0; bid < BUCKET_NUM; bid++) {
            dfb_init_bucket(&(q->mlcq[qid][bid]));
            q->real_qid_cache[qid][bid] = \
                qid + (bid ? __ffs(bid) : BUCKET_NUM_SCALE);
        }
    }
    qdisc_watchdog_init(&q->watchdog, sch);
    return 0;
}

static int dfb_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    struct tc_fifo_qopt opt = { .limit = sch->limit };

    if (nla_put(skb, TCA_OPTIONS, sizeof(opt), &opt))
        goto nla_put_failure;
    return skb->len;

nla_put_failure:
    return -1;
}

static struct Qdisc_ops dfb_qdisc_ops __read_mostly = {
    .id         =    "dfb",
    .priv_size  =    sizeof(struct dfb_sched_data),
    .enqueue    =    dfb_enqueue,
    .dequeue    =    dfb_dequeue,
    .peek       =    qdisc_peek_head,
    .init       =    dfb_init,
    .reset      =    qdisc_reset_queue,
    .destroy    =    dfb_destroy,
    .change     =    dfb_init,
    .dump       =    dfb_dump,
    .owner      =    THIS_MODULE,
};

static int __init dfb_module_init(void)
{
    int ret = 0;
    pr_info("Init module %s.\n", dfb_qdisc_ops.id);
    // TODO: Test whether it is the highest bit
    max_rate_highest_bit = fls64(max_rate);
    ret = register_qdisc(&dfb_qdisc_ops);
    return ret;
}

static void __exit dfb_module_exit(void)
{
    pr_info("Exit module %s.\n", dfb_qdisc_ops.id);
    unregister_qdisc(&dfb_qdisc_ops);
}

module_init(dfb_module_init)
module_exit(dfb_module_exit)
MODULE_AUTHOR("Danfeng Shan");
MODULE_LICENSE("GPL");
