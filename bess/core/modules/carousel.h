//
// Created by admin on 2021/10/8.
//

#ifndef HY_BATCHER_CAROUSEL_H
#define HY_BATCHER_CAROUSEL_H

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include <vector>
#include <unordered_map>
#include <utility>
#include "../commands.h"
#include "../message.h"
#include <hash_map>
#include <iostream>
#include "../kmod/llring.h"
#include "../utils/flow.h"
#include "../utils/mempool-list.h"
#include "../utils/PacketGenerator.h"
#include "../utils/list-bucket.h"
#include <glog/logging.h>
#include <memory>
#include <rte_errno.h>
#include <rte_mempool.h>

#include "../port.h"

#define DEBUG 0
#define CAROUSEL_ENQUEUE_DEBUG 0
#define CAROUSEL_DEQUEUE_DEBUG 0
#define MULTI_BUCKET_DEQUEUE 0
#define CNT_DEBUG 0
#define PACKETS_PER_BUCKETS 32;


using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::be32_t;
using bess::utils::be16_t;

class Carousel final : public Module
{
    using pkts_bucket = ListBucket;
public:
    static const Commands cmds;


    Carousel()
            : Module(), max_dequeue_burst_(), header_ts(), granularity_(), horizon_(), slots(),
              pool_(nullptr), pktgen(nullptr), MAX_BYTES_PER_FLOW()
    {
        is_task_ = true;

        propagate_workers_ = false;
//        max_allowed_workers_ = Worker::kMaxWorkers;
    }

    CommandResponse Init(const bess::pb::CarouselArg &arg);

    CommandResponse AddFlow(const bess::pb::CarouselCommandAddArg &arg);

    CommandResponse SetDefaultRate(const bess::pb::CarouselDefaultRateArg &arg);

    CommandResponse SetMaxDequeueBurst(const bess::pb::CarouselMaxDequeueBurstArg &arg);

    CommandResponse SetGenerateBurst(const bess::pb::CarouselGenerateBurstArg &arg);

    CommandResponse SetGeneratePktSize(const bess::pb::CarouselGeneratePktSizeArg &arg);

    struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *arg) override;

    void Enqueue(Context *ctx, bess::PacketBatch *batch);

    void Dequeue(bess::PacketBatch *batch);

    void DeInit() override;

    std::string GetDesc() const override;


private:

    int Insert(bess::Packet *packet, uint64_t &send_time, uint32_t flow_id);

    std::vector<pkts_bucket *> *queue_;
    std::vector<FlowInfo> flow_info;
    int max_dequeue_burst_ = 32;// dequeue batch size

    uint32_t dequeue_burst = 0;// dequeue batch size
    uint64_t header_ts;
    uint32_t granularity_; // ns
    uint32_t horizon_;
    uint64_t slots;

    MemoryPool<PacketNode> *pool_;
    PacketGenerator *pktgen;

    std::string templates;
    uint32_t generate_burst;

    std::allocator<pkts_bucket> bucket_allocator;
    pkts_bucket *bucket;
    uint64_t packets_num=0;

    uint32_t MAX_BYTES_PER_FLOW; // Bytes

    // queue_out
    Port *port_;
    queue_t qid_;

    struct EnqueueState
    {
        uint64_t pkts_num = 0;
        uint64_t cpu_cycle = 0;
    } enqueue_state;

    struct DequeueState
    {
        uint64_t pkts_num = 0;
        uint64_t cpu_cycle = 0;
    } dequeue_state;

    //Statistics spend cpu cycle per enqueue or dequeue
    uint64_t enqueue_begin_cycle = 0;
    uint64_t enqueue_end_cycle = 0;

    uint64_t dequeue_begin_cycle = 0;
    uint64_t dequeue_end_cycle = 0;
    uint64_t dequeued_batch = 0;
};

#endif //HY_BATCHER_CAROUSEL_H
