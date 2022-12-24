//
// Created by admin on 2021/9/13.
//

#ifndef HY_BATCHER_EIFFEL_H
#define HY_BATCHER_EIFFEL_H

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include <vector>
#include <unordered_map>
#include <utility>
#include "../commands.h"
#include "../message.h"
#include <hash_map>
#include <iostream>
#include "../utils/flow.h"
#include "../utils/mempool-list.h"
#include "../utils/list-bucket.h"
#include "../utils/PacketGenerator.h"
#include <rte_errno.h>
#include <rte_mempool.h>

#include "../port.h"

#define EIFFEL_ENQUEUE_DEBUG  0
#define EIFFEL_DEQUEUE_DEBUG  0
#define DEBUG  0

#define DROP_PACKET_INSERT 1

#define MAIN_QUEUE_ID 0;
#define BUFFER_QUEUE_ID 1;

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::be32_t;
using bess::utils::be16_t;


class Eiffel;

std::pair<long long, int> ffs(Eiffel *eiffel);

class Eiffel final : public Module
{
    using pkts_bucket = ListBucket;
public:
    static const Commands cmds;

    Eiffel()
            : Module(), max_dequeue_burst_(), main_ts(), buffer_ts(), granularity_(), horizon_(),
              slots(), pool_(nullptr), pktgen(nullptr), MAX_BYTES_PER_FLOW()
    {
        is_task_ = true;

        propagate_workers_ = false;
//        max_allowed_workers_ = Worker::kMaxWorkers;

    };

    CommandResponse Init(const bess::pb::EiffelArg &arg);

    CommandResponse AddFlow(const bess::pb::EiffelCommandAddArg &arg);

    CommandResponse SetDefaultRate(const bess::pb::EiffelDefaultRateArg &arg);

    CommandResponse SetMaxDequeueBurst(const bess::pb::EiffelMaxDequeueBurstArg &arg);

    CommandResponse SetGenerateBurst(const bess::pb::EiffelGenerateBurstArg &arg);

    CommandResponse SetGeneratePktSize(const bess::pb::EiffelGeneratePktSizeArg &arg);

    struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *arg) override;

    void Enqueue(Context *ctx, bess::PacketBatch *batch);

    void Dequeue(bess::PacketBatch *batch);

    void AdjustRotation();

    std::string GetDesc() const override;

    void DeInit() override;

private:

    friend std::pair<long long, int> ffs(Eiffel *eiffel);

    int DropPacketInsert(bess::Packet *packet, uint64_t &send_time, uint32_t flow_id);

    std::vector<pkts_bucket *> *main_queue;
    std::vector<pkts_bucket *> *buffer_queue;
    std::vector<FlowInfo> flow_info;
    BitMap *main_map;
    BitMap *buffer_map;

    int max_dequeue_burst_;// dequeue batch size

    uint32_t dequeue_burst = 0;// dequeue batch size

    uint64_t main_ts;
    uint64_t buffer_ts;
    uint32_t granularity_;
    uint32_t horizon_;
    uint64_t slots;

    MemoryPool<PacketNode> *pool_;
    PacketGenerator *pktgen;

    std::string templates;
    uint32_t generate_burst;
//    std::vector<uint32_t> flow_hash;
//    uint16_t pos;

    std::allocator<pkts_bucket> main_bucket_allocator;
    std::allocator<pkts_bucket> buffer_bucket_allocator;
    pkts_bucket *main_bucket;
    pkts_bucket *buffer_bucket;
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

#endif //HY_BATCHER_EIFFEL_H
