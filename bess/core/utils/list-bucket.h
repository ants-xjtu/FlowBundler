//
// Created by admin on 2021/10/29.
//

#ifndef HY_BATCHER_LIST_BUCKET_H
#define HY_BATCHER_LIST_BUCKET_H

#include "mempool-list.h"
#include "iostream"
#include "../packet.h"
#include <hash_map>
#include "circular-queue.h"

#define BUCKET_DEBUG 0
using bess::Packet;
using bess::PacketBatch;
struct PacketNode
{
    Packet *packet = nullptr;
    uint32_t flow_id = 0;
    PacketNode *next = nullptr;
};

class ListBucket
{
public:
    static std::vector<uint64_t> pkts_num;
    static CircularQueue<uint32_t> flow_to_gen;

    ListBucket(MemoryPool<PacketNode> *pool, uint32_t max_bytes_per_flow) : node_num(0)
    {
        pool_ = pool;
        head = new PacketNode;
        MAX_BYTES_PER_FLOW = max_bytes_per_flow;
    }

    inline int dequeue_packet_bulk(PacketBatch *batch, int burst);

    inline int enqueue_packet(Packet *packet, uint32_t flow_id);

    bool IsEmpty() { return node_num == 0; };

    int GetNodeNum() { return node_num; }

    ~ListBucket()
    {
        delete head;
    }

private:
    uint32_t node_num;
    uint32_t MAX_BYTES_PER_FLOW;
    MemoryPool<PacketNode> *pool_;
    PacketNode *head;
};

inline int ListBucket::dequeue_packet_bulk(PacketBatch *batch, int burst)
{
    int quota = burst > node_num ? node_num : burst;
    int dequeue_num = quota;

    PacketNode *pkt;
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
        pkts_num[pkt->flow_id] -= pkt->packet->total_len();
        if (pkts_num[pkt->flow_id] < MAX_BYTES_PER_FLOW)
            flow_to_gen.Push(pkt->flow_id);
        batch->add(pkt->packet);
        quota--;
        head->next = pkt->next;
        pool_->PutElement(pkt);
    }

    dequeue_num -= quota;
    node_num -= dequeue_num;

    return dequeue_num;

}

inline int ListBucket::enqueue_packet(Packet *packet, uint32_t flow_id)
{
    PacketNode *pkt = pool_->GetElement();
    if (pkt == nullptr)
    {
#if BUCKET_DEBUG >= 1
        LOG(INFO) << "not enough room in pool";
#endif
        return -1;
    }
    pkt->packet = packet;
    pkt->flow_id = flow_id;
    pkt->next = head->next;
    head->next = pkt;

    pkts_num[flow_id] += packet->total_len();
    node_num++;
    return 0;
}


#endif //HY_BATCHER_LIST_BUCKET_H
