//
// Created by admin on 2021/9/13.
//

#include "eiffel.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Tcp;

std::pair<long long, int> ffs(Eiffel *eiffel)
{
    auto m_map = eiffel->main_map;
    auto b_map = eiffel->buffer_map;
    auto layers = m_map->n_layers;
    long long index = -1;
    int queue_id = -1;

    // 'seq' Records the location of the first non-zero bit in an uint64 bit-set
    // it also figures out how many sets of uint64 behind this layer.
    int seq = 0;
    if (m_map->bit_map[0][0] not_eq 0)
    {
        queue_id = MAIN_QUEUE_ID;
        seq = _ffs(m_map->bit_map[0][0]);
        index = seq - 1;
        // Records how many sets of uint64 behind the uint64 which contains the
        // non-zero bit in total after finding all layers.
        uint64_t count = 0;
        for (int i = 1; i < layers; ++i)
        {
            count = seq - 1 + (count << 6);
            seq = _ffs(m_map->bit_map[i][count]);
        }

        index = seq - 1 + (count << 6);

        return std::make_pair(index, queue_id);
    }
    if (b_map->bit_map[0][0] not_eq 0)
    {
        queue_id = BUFFER_QUEUE_ID;
        seq = _ffs(b_map->bit_map[0][0]);
        index = seq - 1;
        // Records how many sets of uint64 behind the uint64 which contains the
        // non-zero bit in total after finding all layers.
        uint64_t count = 0;
        for (int i = 1; i < layers; ++i)
        {
            count = seq - 1 + (count << 6);
            seq = _ffs(b_map->bit_map[i][count]);
        }

        index = seq - 1 + (count << 6);

        return std::make_pair(index, queue_id);
    }

    return std::make_pair(-1, -1);

}

const Commands Eiffel::cmds = {
        {"set_max_dequeue_burst", "EiffelMaxDequeueBurstArg", MODULE_CMD_FUNC(
                &Eiffel::SetMaxDequeueBurst), Command::THREAD_SAFE},
        {"set_default_rate",      "EiffelDefaultRateArg",     MODULE_CMD_FUNC(
                &Eiffel::SetDefaultRate),     Command::THREAD_SAFE},
        {"add_flow",              "EiffelCommandAddArg",      MODULE_CMD_FUNC(
                &Eiffel::AddFlow),            Command::THREAD_SAFE},
        {"set_generate_burst",    "EiffelGenerateBurstArg",   MODULE_CMD_FUNC(
                &Eiffel::SetGenerateBurst),   Command::THREAD_SAFE},
        {"set_generate_pkt_size", "EiffelGeneratePktSizeArg", MODULE_CMD_FUNC(
                &Eiffel::SetGeneratePktSize), Command::THREAD_SAFE}
};

void Eiffel::AdjustRotation()
{
    uint64_t tmp_ts = buffer_ts;
    main_ts = buffer_ts;
    buffer_ts = tmp_ts + horizon_;

    auto queue = main_queue;
    main_queue = buffer_queue;
    buffer_queue = queue;

    auto map = main_map;
    main_map = buffer_map;
    buffer_map = map;
}

CommandResponse Eiffel::Init(const bess::pb::EiffelArg &arg)
{
    task_id_t tid;
    tid = RegisterTask(nullptr);
    if (tid == INVALID_TASK_ID)
    {
        return CommandFailure(ENOMEM, "task creation failed");
    }

    uint32_t mempool_size = arg.size();
    pool_ = new MemoryPool<PacketNode>(mempool_size, "eiffel");

    //init MAX_BYTES_PER_FLOW
    MAX_BYTES_PER_FLOW = arg.max_pkts_kbytes() * 1000;

    // init packet generator
    generate_burst = arg.generate_burst();
    if (arg.generate_burst() > bess::PacketBatch::kMaxBurst)
    {
        return CommandFailure(EINVAL, "burst size must be [0,%zu]", bess::PacketBatch::kMaxBurst);
    }

    templates = arg.templates();

    granularity_ = arg.granularity();
    slots = arg.slots();
    horizon_ = granularity_ * slots;

    main_map = new BitMap(slots);
    buffer_map = new BitMap(slots);
    main_queue = new std::vector<pkts_bucket *>;
    buffer_queue = new std::vector<pkts_bucket *>;
    main_queue->resize(slots, nullptr);
    buffer_queue->resize(slots, nullptr);
    main_bucket = main_bucket_allocator.allocate(slots);
    buffer_bucket = buffer_bucket_allocator.allocate(slots);
    for (uint64_t i = 0; i < slots; ++i)
    {
        (*main_queue)[i] = main_bucket + i;
        (*buffer_queue)[i] = buffer_bucket + i;
        main_bucket_allocator.construct(main_bucket + i, pool_, MAX_BYTES_PER_FLOW);
        buffer_bucket_allocator.construct(buffer_bucket + i, pool_, MAX_BYTES_PER_FLOW);
    }

    main_ts = tsc_to_ns(rdtsc());
    buffer_ts = main_ts + horizon_;

    // init queue_out
    const char *port_name;
    int ret;

    if (!arg.port().length()) {
        return CommandFailure(EINVAL, "Field 'port' must be specified");
    }

    port_name = arg.port().c_str();
    qid_ = arg.qid();

    const auto &it = PortBuilder::all_ports().find(port_name);
    if (it == PortBuilder::all_ports().end()) {
        return CommandFailure(ENODEV, "Port %s not found", port_name);
    }
    port_ = it->second;

    node_constraints_ = port_->GetNodePlacementConstraint();

    ret = port_->AcquireQueues(reinterpret_cast<const module *>(this),
                             PACKET_DIR_OUT, &qid_, 1);
    if (ret < 0) {
        return CommandFailure(-ret);
    }

    return CommandSuccess();
}

CommandResponse Eiffel::AddFlow(const bess::pb::EiffelCommandAddArg &arg)
{
    uint32_t flow_num = arg.flows_size() > 0 ? arg.flows_size() : arg.flow_num();
    uint64_t now = tsc_to_ns(rdtsc());
    flow_info.resize(flow_num);
    pkts_bucket::pkts_num.resize(flow_num, 0);
    pkts_bucket::flow_to_gen.Reset();
    if (arg.flows_size() > 0)
    {
        for (uint32_t i = 0; i < flow_num; ++i)
        {
            const auto flow = arg.flows(i);
            uint32_t rate_ = flow.rate();

            flow_info[i].rate = rate_;
            flow_info[i].last_stamp = now;
            pkts_bucket::pkts_num[i] = 0;
        }
    }
    else
    {
        uint32_t rate_ = arg.total_rate() / flow_num;
        for (uint32_t i = 0; i < flow_num; ++i)
        {
            flow_info[i].rate = rate_;
            flow_info[i].last_stamp = tsc_to_ns(rdtsc());
            pkts_bucket::pkts_num[i] = 0;
        }
    }

    // Init flow_to_gen
    for (int i = 0; i < MAX_BYTES_PER_FLOW / 1500; i++)
    {
        for (int j = 0; j < flow_num; j++)
        {
            pkts_bucket::flow_to_gen.Push(j);
        }
    }

    // Init pktgen
    uint32_t flows_per_dst = arg.flows_per_dst();
    pktgen = new PacketGenerator(generate_burst, templates, flow_num, flows_per_dst);
    return CommandSuccess();
}

CommandResponse Eiffel::SetDefaultRate(const bess::pb::EiffelDefaultRateArg &arg)
{
    return CommandSuccess();
}

CommandResponse Eiffel::SetMaxDequeueBurst(const bess::pb::EiffelMaxDequeueBurstArg &arg)
{
    max_dequeue_burst_ =
            arg.max_dequeue_burst() > (int) bess::PacketBatch::kMaxBurst ? (int) bess::PacketBatch::kMaxBurst
                                                                         : arg.max_dequeue_burst();
    return CommandSuccess();
}

CommandResponse Eiffel::SetGenerateBurst(const bess::pb::EiffelGenerateBurstArg &arg)
{
    pktgen->SetGenerateBurst(arg.generate_burst());
    return CommandSuccess();
}

CommandResponse Eiffel::SetGeneratePktSize(const bess::pb::EiffelGeneratePktSizeArg &arg)
{
    pktgen->SetGenerateSize(arg.generate_pkt_size());
    return CommandSuccess();
}

struct task_result Eiffel::RunTask(Context *ctx, bess::PacketBatch *batch, void *)
{
    // for queue_out
    Port *p = port_;
    const queue_t qid = qid_;
    uint64_t sent_bytes = 0;
    int sent_pkts = 0;

    uint64_t total_bytes = 0;
    if (children_overload_ > 0)
    {
        return {.block = true, .packets = 0, .bits = 0};
    }

#if DEBUG >= 1
    LOG(INFO) << "dequeue_burst = " << dequeue_burst;
    LOG(INFO) << "enqueue_round = " << enqueue_round;
#endif
    // Generate, rewrite & enqueue pkts
    if (!pkts_bucket::flow_to_gen.IsEmpty())
    {
        if (!pktgen->AllocPackets(batch))
        { // Cannot generate pkts
            return {.block = true, .packets = 0, .bits = 0};
        } else
        {
            pktgen->RewriteUsingSingleTemplate(batch);
//            enqueue_begin_cycle = rdtsc();
            Enqueue(ctx, batch);
//            enqueue_end_cycle = rdtsc();
//            enqueue_state.cpu_cycle += enqueue_end_cycle - enqueue_begin_cycle;
//            enqueue_state.pkts_num += batch->cnt();
        }
    }

    dequeue_begin_cycle = rdtsc();
    // Dequeue pkts
#if REPEATED_FFS_DEQUEUE == 0
    Dequeue(batch);
#else
    //    RepeatedFFSDequeue(batch);
#endif
    // Check if pkt(s) really dequeued
    const int cnt = batch->cnt();

    if (cnt == 0)
    {
        return {.block = true, .packets = 0, .bits = 0};
    } else
    {
        if (p->conf().admin_up) {
            sent_pkts = p->SendPackets(qid, batch->pkts(), batch->cnt());
            if (sent_pkts > 0)
            {
                dequeue_end_cycle = rdtsc();
                dequeued_batch ++;
                dequeue_state.cpu_cycle += dequeue_end_cycle - dequeue_begin_cycle;
                dequeue_state.pkts_num += sent_pkts;
            }
            packets_num-=cnt;
        }

        if (!(p->GetFlags() & DRIVER_FLAG_SELF_OUT_STATS)) {
            const packet_dir_t dir = PACKET_DIR_OUT;

            for (int i = 0; i < sent_pkts; i++) {
                sent_bytes += batch->pkts()[i]->total_len();
            }

            p->queue_stats[dir][qid].packets += sent_pkts;
            p->queue_stats[dir][qid].dropped += (batch->cnt() - sent_pkts);
            p->queue_stats[dir][qid].bytes += sent_bytes;
        }

        // Calculate the total dequeued pkts bytes
        for (int i = 0; i < sent_pkts; ++i) {
            total_bytes += batch->pkts()[i]->total_len();
        }
        //RunNextModule(ctx, batch);

        if (sent_pkts < batch->cnt()) {
            bess::Packet::Free(batch->pkts() + sent_pkts, batch->cnt() - sent_pkts);
        }
        return {
                .block = false,
                .packets = (uint32_t) cnt,
                .bits = (total_bytes) * 8
        };
    }
}

int Eiffel::DropPacketInsert(bess::Packet *packet, uint64_t &send_time, uint32_t flow_id)
{
    uint64_t index;
    std::vector<pkts_bucket *> *queue;
    if (send_time < buffer_ts)
    {
        queue = main_queue;
        if (send_time < main_ts)
        {
            index = 0;
            send_time = main_ts;
        } 
        else
            index = (send_time - main_ts) / granularity_;
        SetBitMap(index, main_map);
    } else
    {
        queue = buffer_queue;
        if (send_time >= main_ts + horizon_ + horizon_)
            return -1;
        else
            index = (send_time - buffer_ts) / granularity_;
        SetBitMap(index, buffer_map);
    }

    int enqueue_num = (*queue)[index]->enqueue_packet(packet, flow_id);
    return enqueue_num;
}

void Eiffel::Enqueue(Context *ctx, bess::PacketBatch *batch)
{
#if EIFFEL_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "===============enqueue begin===============";
#endif
    int cnt = batch->cnt();
    if (cnt < 1)
        return;

    bess::Packet * pkt;
    Ethernet *eth;
    Ipv4 *ip;
    Udp *udp;
    Tcp *tcp;
    uint32_t pkt_bytes;
    uint64_t rate;
    uint64_t lst_stamp;
    uint64_t new_stamp;
    uint64_t now;
    uint32_t flow_id;
    int enqueue_result;


    enqueue_begin_cycle = rdtsc();
    for (int i = 0; i < cnt; ++i)
    {
        now = tsc_to_ns(rdtsc());
        pkt = batch->pkts()[i];
        pkt_bytes = pkt->total_len();
        eth = pkt->head_data<Ethernet *>();
        ip = reinterpret_cast<Ipv4 *>(eth + 1);
        flow_id = ip->src.value();

        FlowInfo &info = flow_info[flow_id];
        if (info.rate == 0)
        {
            info.rate = DEFAULT_FLOW_RATE;
            info.last_stamp = now;
        }
        rate = info.rate;

        //calculate send timestamp
        lst_stamp = info.last_stamp;
        new_stamp = lst_stamp + (pkt_bytes << 3) * RATE_SCALE / rate;
        new_stamp = new_stamp >= now ? new_stamp : now;
        enqueue_result = DropPacketInsert(pkt, new_stamp, flow_id);
        if (enqueue_result < 0)
        {
            DropPacket(ctx, pkt);
        } else
        {
            packets_num++;
            info.last_stamp = new_stamp;
            enqueue_state.pkts_num ++;
        }
#if EIFFEL_ENQUEUE_DEBUG >= 1
        LOG(INFO) << "lst_stamp = " << lst_stamp << ", new_stamp = " << new_stamp << ", pkt_bytes = " << pkt_bytes
                  << ", rate = " << rate << ", now = " << now;
#endif
    }
    enqueue_end_cycle = rdtsc();
    enqueue_state.cpu_cycle += enqueue_end_cycle - enqueue_begin_cycle;
}

void Eiffel::Dequeue(bess::PacketBatch *batch)
{
#if EIFFEL_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "===============dequeue begin===============";
#endif
    long long index;
    uint64_t send_ts;
    uint64_t now;
    int burst = ACCESS_ONCE(max_dequeue_burst_);
    int queue_id;
    std::vector<pkts_bucket *> *queue;
    BitMap *map;
    batch->clear();

    dequeue_burst = 0;

    now = tsc_to_ns(rdtsc());
    auto ffs_result = ffs(this);
    index = ffs_result.first;
    queue_id = ffs_result.second;

    //adjust rotation
    if (queue_id == -1)
    {
        main_ts = now;
        buffer_ts = now + horizon_;
        return;
    } else if (queue_id == 1)
    {
        AdjustRotation();
    }
    send_ts = index * granularity_ + main_ts;

    queue = main_queue;
    map = main_map;
    if (send_ts > now)
    {
#if EIFFEL_DEQUEUE_DEBUG >= 1
        LOG(INFO) << "dequeue return for send ts > now";
        LOG(INFO) << "ffs index: " << index << "  ffs queue_id: " << queue_id;
        LOG(INFO) << "bucket packet num: "<< (*queue)[index]->GetNodeNum();
        LOG(INFO) << "send ts: "<<send_ts<<"  now:"<<now;
#endif
        return;
    }

#if EIFFEL_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "before dequeue packets num: " << (*queue)[index]->GetNodeNum();
#endif
    int dequeued = (*queue)[index]->dequeue_packet_bulk(batch, burst);
#if EIFFEL_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "dequeue packets num: " << dequeued;
    LOG(INFO) << "after dequeue packets num" << (*queue)[index]->GetNodeNum();
#endif
    batch->set_cnt(dequeued);
    dequeue_burst = (uint32_t) dequeued;

    if ((*queue)[index]->IsEmpty())
        FreeBitMap(index, map);
}

std::string Eiffel::GetDesc() const
{
    double avg_cycle_enqueue_pkt = (double) enqueue_state.cpu_cycle / (double) enqueue_state.pkts_num;
    double avg_cycle_dequeue_pkt = (double) dequeue_state.cpu_cycle / (double) dequeue_state.pkts_num;
    double avg_dequeue_size = (double)dequeue_state.pkts_num / ACCESS_ONCE(dequeued_batch);
    uint64_t pkts_num = ACCESS_ONCE(packets_num);
    return bess::utils::Format(
            "enqueue status:\\n"
            "avg cpu cycle consume per pkt: %.6lf\\n"
            "total enqueued pkts number: %lu\\n"
            "dequeue status:\\n"
            "avg cpu cycle consume per pkt: %.6lf\\n"
            "total dequeued pkts number: %lu\\n"
            "avg dequeue size: %.6lf\\n"
            "packets num in queue: %lu\\n"
            "drop num: %llu\\n"
            "Queue_out Status:\\n"
            "%s:%hhu/%s",
            avg_cycle_enqueue_pkt,
            enqueue_state.pkts_num,
            avg_cycle_dequeue_pkt,
            dequeue_state.pkts_num,
            avg_dequeue_size,
            packets_num,
            port_->queue_stats[PACKET_DIR_OUT][qid_].dropped,
            port_->name().c_str(), qid_,
            port_->port_builder()->class_name().c_str()
    );
}

void Eiffel::DeInit()
{
    delete pool_;
    main_bucket_allocator.deallocate(main_bucket, slots);
    buffer_bucket_allocator.deallocate(buffer_bucket, slots);
    delete main_queue;
    delete buffer_queue;
    delete main_map;
    delete buffer_map;
    delete pktgen;

    if (port_) {
        port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_OUT,
                         &qid_, 1);
    }
}

ADD_MODULE(Eiffel, "eiffel", "traffic shaping")
