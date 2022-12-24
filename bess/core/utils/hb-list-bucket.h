//
// Created by admin on 2021/11/29.
//

#ifndef HY_BATCHER_HB_LIST_BUCKET_H
#define HY_BATCHER_HB_LIST_BUCKET_H
#include "mempool-list.h"
#include "iostream"
#include "../packet.h"
#include <hash_map>
#include <list>
#include "easy-map.h"
#include "circular-queue.h"
#include <unordered_map>
#include "flow_rate_estimator.h"

#define BUCKET_DEBUG 0
using bess::Packet;
using bess::PacketBatch;
struct HBPacketNode
{
    Packet *packet = nullptr;
    uint32_t flow_id = 0;
//    uint32_t bucket_id = 0;
    uint64_t send_ts = 0;
    uint32_t hash_id = 0;        // hash value of aggregation_rates
    HBPacketNode *next = nullptr;
};

class HBListBucket
{
public:
    static std::vector<uint64_t> pkts_num;
    // easy map
    static EasyMap aggregation_rates;

    static CircularQueue<uint32_t> flow_to_gen;
    HBListBucket(MemoryPool<HBPacketNode> *pool, uint32_t max_bytes_per_flow) : node_num(0)
    {
        pool_ = pool;
        head = new HBPacketNode;
        MAX_BYTES_PER_FLOW = max_bytes_per_flow;
    }

    inline int dequeue_packet_bulk(PacketBatch *batch, int burst, uint32_t bucket_id);

    inline int enqueue_packet(Packet *packet, uint32_t flow_id, uint64_t send_ts ,uint32_t hash_id);

    bool IsEmpty() { return node_num == 0; };

    int GetNodeNum() { return node_num; }

    ~HBListBucket()
    {
        delete head;
    }

private:
    uint32_t node_num;
    uint32_t MAX_BYTES_PER_FLOW;
    MemoryPool<HBPacketNode> *pool_;
    HBPacketNode *head;
};

inline int HBListBucket::dequeue_packet_bulk(PacketBatch *batch, int burst, uint32_t bucket_id)
{
    int quota = burst > node_num ? node_num : burst;
    int dequeue_num = quota;
    uint64_t left_bytes;

    HBPacketNode *pkt;
    if (head->next == nullptr)
    {
#if BUCKET_DEBUG >= 1
        LOG(INFO) << "head->nex == null";
#endif
        return 0;
    }
    while (quota > 0)
    {
        pkt = head->next;
        left_bytes = pkts_num[pkt->flow_id] - pkt->packet->total_len();
        if (left_bytes < MAX_BYTES_PER_FLOW)
            flow_to_gen.Push(pkt->flow_id);
        pkts_num[pkt->flow_id] = left_bytes;
        estimator.UpdateRate(pkt->packet->total_len(), pkt->send_ts, pkt->hash_id, bucket_id);
        batch->add(pkt->packet);
        quota--;
        head->next = pkt->next;
        pool_->PutElement(pkt);
    }

    dequeue_num -= quota;
    node_num -= dequeue_num;

    return dequeue_num;

}

inline int HBListBucket::enqueue_packet(Packet *packet, uint32_t flow_id, uint64_t send_ts, uint32_t hash_id)
{
    HBPacketNode *pkt = pool_->GetElement();
    if (pkt == nullptr)
    {
#if BUCKET_DEBUG >= 1
        LOG(INFO) << "not enough room in pool";
#endif
        return -1;
    }
    pkt->packet = packet;
    pkt->flow_id = flow_id;
//    pkt->dst_ip = dst_ip;
    pkt->send_ts = send_ts;
    pkt->next = head->next;
    pkt->hash_id = hash_id;
    head->next = pkt;

    node_num++;
    return 0;
}

#endif //HY_BATCHER_HB_LIST_BUCKET_H
