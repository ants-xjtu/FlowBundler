/*
 * FlowBundler (Multi-level-queue-based)
 */

#ifndef BESS_MODULES_FLOWBUNDLER_H
#define BESS_MODULES_FLOWBUNDLER_H

#include "../module.h"
#include "../commands.h"
#include "../message.h"
#include "../pb/module_msg.pb.h"
#include "../utils/flow.h"
#include "../utils/mempool-list.h"
#include "../utils/PacketGenerator.h"
#include "../utils/hb-list-bucket.h"
#include "../utils/flow_rate_estimator.h"
#include <vector>
#include <utility>
#include <iostream>
#include <unordered_map>
#include <glog/logging.h>
#include <memory>
#include <rte_errno.h>
#include <rte_mempool.h>

#include "../port.h"

#define HB_DEBUG 0

#define HB_INIT_DEBUG 0
#define HB_ENQUEUE_DEBUG 0
#define HB_ADDFLOW_DEBUG 0
#define HB_DEQUEUE_DEBUG 0
#define HB_MULTI_BUCKET_DEQUEUE 0

#define QUEUE_NUM 64U
#define MTU 1500

// Returns one plus the index of the least significant 1-bit
// #define _ffs(a) __builtin_ffsll(a)

// Returns the number of leading 0-bits
#define _fls(a) __builtin_clzll(a)

/*
 * Note that the "+1".
 * Without "+1", the packet may be put in to a bucket
 * whose expected send time is smaller than the head_time.
 * As a result, the bucket will be visited in the next round.
 * This is very long time if the pacing rate is very low
 */
//#define FindBucketIndex(queue_id, send_time) \
        (((send_time) >> ((queue_id) + min_gran_scale)) + 1)

#define HeadTimeMask(queue_id) \
        ((~((uint64_t) 0)) << (queue_id))

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::be32_t;
using bess::utils::be16_t;

struct Location
{
    uint32_t queue_id;
    uint64_t bucket_id;
};

class FlowBundler final : public Module
{
    using pkts_bucket = HBListBucket;
public:
    static const Commands cmds;

    FlowBundler() :
            Module(), pool_(nullptr), pktgen(nullptr), MAX_BYTES_PER_FLOW(),
            port_(), qid_()
    {
        is_task_ = true;
        propagate_workers_ = false;
//        max_allowed_workers_ = Worker::kMaxWorkers;
    }

    CommandResponse Init(const bess::pb::FlowBundlerArg &arg);

    CommandResponse SetDefaultRate(const bess::pb::EiffelDefaultRateArg &arg);

    CommandResponse AddFlow(const bess::pb::EiffelCommandAddArg &arg);

//    void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
    struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *arg) override;

    void DeInit() override;

    std::string GetDesc() const override;

private:
    inline uint32_t FindQueueIndex(uint64_t rate)
    {
        uint32_t id = 0;
        rate = rate < max_rate ? rate : max_rate;
        id = _fls(rate) - max_rate_highest_bit;
        return id;
    }

    inline uint64_t FindBucketIndex(uint32_t queue_id, uint64_t send_time)
    {
        uint64_t bucket_id = send_time >> (queue_id + min_gran_scale);
        bucket_id = (bucket_id << (queue_id + min_gran_scale)) < send_time
                        ? bucket_id + 1 : bucket_id;
        return bucket_id;
    }


    struct Location FindRealLocation(uint32_t queue_id, uint64_t send_time);

    int Insert(bess::Packet *packet, uint32_t queue_id, uint64_t send_time,
               uint32_t flow_id, uint64_t rate, uint32_t hash_id);

    void Enqueue(Context *ctx, bess::PacketBatch *batch);

    void Dequeue(bess::PacketBatch *batch);

    // scaling factor of bucket number
    uint8_t bucket_num_scale;
    uint64_t bucket_num;        // bucket_num = (1 << bucket_num_scale)

    // scaling factor of minimum granularity (in ns)
    uint8_t min_gran_scale;
    uint64_t min_gran;          // min_gran = 1 << min_gran_scale

    // This mask is used to round bucket index: bucket_id = bucket_id & bid_msak
    uint32_t bid_mask;          // bid_mask = (1 << bucket_num_scale) - 1

    // Maximum supported pacing rate, in byte per second
    uint64_t max_rate;          // max_rate = NSEC_PER_SEC * MTU >> min_gran_scale
    uint32_t max_rate_highest_bit;

    uint32_t default_rate = 500;    // Mbps

    // Packet generation
    PacketGenerator *pktgen;
    std::string templates;
    uint32_t generate_burst;

    // Memory pool
    MemoryPool<HBPacketNode> *pool_;
//    std::vector<uint32_t> flow_hash;
//    uint16_t pos = 0;
    std::allocator<pkts_bucket> bucket_allocator[QUEUE_NUM];
    pkts_bucket *bucket[QUEUE_NUM];

    uint32_t MAX_BYTES_PER_FLOW; // Bytes

    // multi-level calendar queue
    std::vector<std::vector<pkts_bucket *> > *m_queue;

    uint64_t n_packets_total;
    uint32_t n_packets[QUEUE_NUM];       // # of packets in each queue
    uint64_t bitmap;                     // each bit indicates whether a queue is empty
    uint64_t head_time;                  // head time in ns
    uint32_t highest_rate_qid;           // the queue id with the highest pacing rate among all active flows
    uint32_t dequeue_burst;              // max dequeue num per round

    uint32_t last_dequeued = 0;          // dequeued num of last round

    std::vector<FlowInfo> flow_info;

    // queue_out
    Port *port_;
    queue_t qid_;

    /// for enqueue
    uint64_t total_enqueue_pkts_num = 0L;
    uint64_t total_enqueue_batch_num = 0L;
    uint64_t total_enqueue_cpu_cycle = 0L;

    /// for dequeue
    uint64_t total_dequeue_pkts_num = 0L;
    uint64_t total_dequeue_batch_num = 0L;
    uint64_t total_dequeue_cpu_cycle = 0L;

    uint64_t dequeued_batch = 0;

};

#endif
