#include "flowbundler.h"
#include <algorithm>

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Tcp;
using bess::utils::be32_t;
using bess::utils::be16_t;

const Commands FlowBundler::cmds = {
        {"set_default_rate", "EiffelDefaultRateArg", MODULE_CMD_FUNC(&FlowBundler::SetDefaultRate), Command::THREAD_SAFE},
        {"add_flow",         "EiffelCommandAddArg",  MODULE_CMD_FUNC(&FlowBundler::AddFlow),        Command::THREAD_SAFE}
};

CommandResponse FlowBundler::Init(const bess::pb::FlowBundlerArg &arg)
{
#if HB_INIT_DEBUG >= 1
    LOG(INFO) << "=============FlowBundler Init================";
#endif

    task_id_t tid;
    tid = RegisterTask(nullptr);
    if (tid == INVALID_TASK_ID)
    {
        return CommandFailure(ENOMEM, "task creation failed");
    }

    bucket_num_scale = arg.bucket_num_scale();
    bucket_num = (1 << bucket_num_scale);

#if HB_INIT_DEBUG >= 1
    LOG(INFO) << "bucket_num = " << bucket_num << ", bucket_size = " << bucket_size;
#endif

    min_gran_scale = arg.min_gran_scale();
    min_gran = 1 << min_gran_scale;

    bid_mask = (1 << bucket_num_scale) - 1;

    max_rate = (RATE_SCALE * (MTU << 3)) >> min_gran_scale;
    max_rate_highest_bit = _fls(max_rate);

    bitmap = 0;
    n_packets_total = 0;
    head_time = tsc_to_ns(rdtsc());
    highest_rate_qid = QUEUE_NUM - 1;
    dequeue_burst = arg.dequeue_burst();

#if HB_INIT_DEBUG >= 1
    LOG(INFO) << "head_time = " << head_time << ", max_rate_highest_bit = " << max_rate_highest_bit;
    LOG(INFO) << "min_gran = " << min_gran << ", dequeue_burst = " << dequeue_burst;
    LOG(INFO) << "max_rate = " << max_rate << ", highest_rate_qid = " << highest_rate_qid;
#endif

    // init mempool
    uint32_t mempool_size = arg.mempool_size();
    pool_ = new MemoryPool<HBPacketNode>(mempool_size, "FlowBundler");

    // init MAX_BYTES_PER_FLOW
    MAX_BYTES_PER_FLOW = arg.max_pkts_kbytes() * 1000;

    // init packet generator
    generate_burst = arg.generate_burst();
    if (arg.generate_burst() > bess::PacketBatch::kMaxBurst)
    {
        return CommandFailure(EINVAL, "burst size must be [0,%zu]", bess::PacketBatch::kMaxBurst);
    }

    // packet generation
    templates = arg.templates();

    // init queue
    m_queue = new std::vector<std::vector<pkts_bucket *> >;
    m_queue->resize(QUEUE_NUM);
    for (int i = 0; i < QUEUE_NUM; i++)
    {
        n_packets[i] = 0;
        bucket[i] = bucket_allocator[i].allocate(bucket_num);
        (*m_queue)[i].resize(bucket_num, nullptr);
        for (int j = 0; j < (int) bucket_num; j++)
        {
            (*m_queue)[i][j] = bucket[i] + j;
            bucket_allocator[i].construct(bucket[i] + j, pool_, MAX_BYTES_PER_FLOW);
        }
    }

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

#if HB_INIT_DEBUG >= 1
    LOG(INFO) << "=============FlowBundler Init End================";
#endif

    return CommandSuccess();
}

CommandResponse FlowBundler::SetDefaultRate(const bess::pb::EiffelDefaultRateArg &arg)
{
    default_rate = arg.default_rate();
    return CommandSuccess();
}

CommandResponse FlowBundler::AddFlow(const bess::pb::EiffelCommandAddArg &arg)
{
    uint32_t flow_num = arg.flows_size() > 0 ? arg.flows_size() : arg.flow_num();
    uint64_t now = tsc_to_ns(rdtsc());
    flow_info.resize(flow_num);
    pkts_bucket::pkts_num.resize(flow_num, 0);
    pkts_bucket::flow_to_gen.Reset();
    pkts_bucket::aggregation_rates.Reset();     // easy map
#if HB_ADDFLOW_DEBUG >= 1
    LOG(INFO) << "=============Add Flow================" << std::endl;
#endif
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
    for (uint32_t i = 0; i < MAX_BYTES_PER_FLOW / 1500; i++)
    {
        for(uint32_t j = 0; j < flow_num; j++)
        {
            pkts_bucket::flow_to_gen.Push(j);
        }
    }

    // Init pktgen
    uint32_t flows_per_dst = arg.flows_per_dst();
    pktgen = new PacketGenerator(generate_burst, templates, flow_num, flows_per_dst);

    return CommandSuccess();
}

struct task_result FlowBundler::RunTask(Context *ctx, bess::PacketBatch *batch, void *)
{
    // for queue_out
    Port *p = port_;
    const queue_t qid = qid_;
    uint64_t sent_bytes = 0;
    int sent_pkts = 0;

    uint64_t dequeue_begin_cycle;
    uint64_t dequeue_end_cycle;

    uint64_t total_bytes = 0;
    if (children_overload_ > 0)
    {
        return {.block = true, .packets = 0, .bits = 0};
    }

#if HB_DEBUG >= 1
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
//            uint64_t enqueue_begin_cycle = rdtsc();
            Enqueue(ctx, batch);
//            total_enqueue_cpu_cycle += rdtsc() - enqueue_begin_cycle;
//            total_enqueue_pkts_num += batch->cnt();
//            total_enqueue_batch_num++;
        }
    }

    // Dequeue pkts
    dequeue_begin_cycle = rdtsc();
    Dequeue(batch);

    // Check if pkt(s) really dequeued
    const int cnt = batch->cnt();

#if HB_CNT_DEBUG >= 1
    LOG(INFO)<<"dequeue cnt: "<<cnt;
    LOG(INFO)<<"head: "<<header_ts % slots;
#endif

    const int pkt_overhead = 24;
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
                total_dequeue_cpu_cycle += dequeue_end_cycle - dequeue_begin_cycle;
                total_dequeue_batch_num++;
                total_dequeue_pkts_num += sent_pkts;
            }
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

//        if (sent_pkts < batch->cnt()) {
//            bess::Packet::Free(batch->pkts() + sent_pkts, batch->cnt() - sent_pkts);
//        }

        // Calculate the total dequeued pkts bytes
        for (int i = 0; i < cnt; ++i)
        {
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

inline struct Location FlowBundler::FindRealLocation(uint32_t queue_id, uint64_t send_time)
{
    struct Location real_loc = {
            .queue_id = 0,
            .bucket_id = 0,
    };
    uint64_t bid = FindBucketIndex(queue_id, send_time);

    real_loc.queue_id = queue_id + _ffs(bid | 1L << (QUEUE_NUM - 1)) - 1;
    if ((real_loc.queue_id) >= QUEUE_NUM)
    {
        real_loc.queue_id = QUEUE_NUM - 1;
    }
    real_loc.bucket_id = FindBucketIndex(real_loc.queue_id, send_time) & bid_mask;
//    LOG(INFO) << "real_loc: queue_id = " << real_loc.queue_id << ", bucket_id = " << real_loc.bucket_id;
    return real_loc;
}

inline int FlowBundler::Insert(bess::Packet *packet, uint32_t queue_id, uint64_t send_time,
                             uint32_t flow_id, uint64_t rate, uint32_t hash_id)
{
#if HB_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "===============Insert Begin===============";
#endif
    uint64_t granularity = 1L << (queue_id + min_gran_scale);
    uint64_t horizon = (granularity << bucket_num_scale) - granularity;

#if HB_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "granularity = " << granularity << ", horizon = " << horizon;
#endif

    // find real location
    struct Location real_loc = {
            .queue_id = 0,
            .bucket_id = 0,
    };

    // first packet
    if (bitmap == 0)
    {
        head_time = send_time & HeadTimeMask(queue_id);
        highest_rate_qid = queue_id;
    }

    uint64_t actual_time= (send_time < head_time) ? head_time : send_time;
    if (actual_time > head_time+horizon)
        queue_id = queue_id+ sizeof(long long )*8 - _fls((actual_time - head_time) >> (queue_id + min_gran_scale + bucket_num_scale));
    real_loc = FindRealLocation(queue_id, actual_time);

    int enqueue_result = (*m_queue)[real_loc.queue_id][real_loc.bucket_id]->enqueue_packet(packet, flow_id, send_time, hash_id);

    // set the bitmap
    if (enqueue_result == 0)
    {
        n_packets[real_loc.queue_id]++;
        n_packets_total++;
        bitmap |= (1L << real_loc.queue_id);
    }
    return enqueue_result;
}

void FlowBundler::Enqueue(Context *ctx, bess::PacketBatch *batch)
{
#if HB_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "===============Enqueue Begin===============";
#endif

    int cnt = batch->cnt();
    if (cnt < 1)
        return;

    Ipv4 *ip;
    Udp *udp;
    Tcp *tcp;
    Ethernet *eth;
    uint32_t dst_ip;

    bess::Packet * pkt;
    uint32_t pkt_bytes;
    uint32_t hash_id = 0;
    uint64_t left_bytes;
    uint64_t rate;
    uint64_t aggre_rate;
    uint64_t lst_stamp;
    uint64_t new_stamp;
    uint32_t flow_id;
    uint64_t now;

    int enqueue_result;


    uint64_t enqueue_begin_cycle = rdtsc();
    for (int i = 0; i < cnt; ++i)
    {
        now = tsc_to_ns(rdtsc());
        pkt = batch->pkts()[i];
        pkt_bytes = pkt->total_len();
        eth = pkt->head_data<Ethernet *>();
        ip = reinterpret_cast<Ipv4 *>(eth + 1);
        flow_id = ip->src.value();
        dst_ip = ip->dst.value();

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
#if HB_ENQUEUE_DEBUG >= 1
        LOG(INFO) << "lst_stamp = " << lst_stamp << ", new_stamp = " << new_stamp;
        LOG(INFO) << "pkt_bytes = " << pkt_bytes << ", rate = " << rate << ", now = " << now;
#endif

        new_stamp = new_stamp >= now ? new_stamp : now;
        left_bytes = pkts_bucket::pkts_num[flow_id];

        // easy map
        aggre_rate = pkts_bucket::aggregation_rates.Get(dst_ip, hash_id);
        // hash map
        // aggre_rate = pkts_bucket::aggregation_rates[dst_ip];

        // unordered map
        // aggre_rate = pkts_bucket::aggregation_rates[dst_ip];

        uint32_t queue_id = FindQueueIndex(aggre_rate);
        highest_rate_qid = queue_id < highest_rate_qid ? queue_id : highest_rate_qid;
        enqueue_result = Insert(pkt, queue_id, new_stamp, flow_id, rate, hash_id);
#if HB_ENQUEUE_DEBUG >= 1
        LOG(INFO) << "enqueue_result = " << enqueue_result;
#endif
        if (enqueue_result < 0)
        {
            DropPacket(ctx, pkt);
        } else
        {
            pkts_bucket::pkts_num[flow_id] = left_bytes + pkt_bytes;
            total_enqueue_pkts_num ++;
            info.last_stamp = new_stamp;
        }
    }
    total_enqueue_cpu_cycle += rdtsc() - enqueue_begin_cycle;
    total_enqueue_batch_num++;
}

void FlowBundler::Dequeue(bess::PacketBatch *batch)
{
#if HB_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "===============Dequeue Begin===============";
#endif
    batch->clear();
    if (!n_packets_total)
    {
        head_time = tsc_to_ns(rdtsc());
        return;
    }

    uint32_t burst = ACCESS_ONCE(dequeue_burst);
    head_time = (head_time) & HeadTimeMask(highest_rate_qid);
    uint64_t granularity = 1L << (highest_rate_qid + min_gran_scale);

    struct Location real_loc;
    uint64_t now = tsc_to_ns(rdtsc());

#if HB_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "now = " << now << ", granularity = " << granularity;
    LOG(INFO) << "highest_rate_qid = " << highest_rate_qid << ", head_time = " << head_time;
#endif

    uint32_t n_packets_total_last = n_packets_total;

    int dequeue_num;
    while (now >= head_time)
    {
        real_loc = FindRealLocation(highest_rate_qid, head_time);

#if HB_DEQUEUE_DEBUG >= 1
        LOG(INFO) << "head_time = " << head_time << ", granularity = " << granularity;
        LOG(INFO) << "queue_id = " << real_loc.queue_id << ", bucket_id = " << real_loc.bucket_id;
#endif

        //int burst = ACCESS_ONCE(max_dequeue_burst_);
        if (n_packets[real_loc.queue_id] > 0 && !(*m_queue)[real_loc.queue_id][real_loc.bucket_id]->IsEmpty())
        {
            dequeue_num = (*m_queue)[real_loc.queue_id][real_loc.bucket_id]->dequeue_packet_bulk(batch, burst, real_loc.bucket_id);

            if (dequeue_num > 0)
            {
                // update packets num and bitmap
                n_packets_total -= dequeue_num;
                n_packets[real_loc.queue_id] -= dequeue_num;
                if (n_packets[real_loc.queue_id] == 0)
                {
                    bitmap &= ~(1L << real_loc.queue_id);
                    highest_rate_qid = bitmap ? (_ffs(bitmap) - 1) : QUEUE_NUM - 1;
                }
                break;
            }
        }
        head_time += granularity;
    }
    // head_time = now & HeadTimeMask(highest_rate_qid);
    last_dequeued = dequeue_num;
    batch->set_cnt(last_dequeued);

#if HB_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "last_dequeued = " << last_dequeued;
#endif
}

std::string FlowBundler::GetDesc() const
{
    double avg_enqueue_cycle_per_pkt = 0.0;

    avg_enqueue_cycle_per_pkt = (double) total_enqueue_cpu_cycle / (double) total_enqueue_pkts_num;

    double avg_dequeue_batch_size = 0.0;
    double avg_dequeue_cycle_per_pkt = 0.0;

    avg_dequeue_batch_size = (double) total_dequeue_pkts_num / (double) total_dequeue_batch_num;
    avg_dequeue_cycle_per_pkt = (double) total_dequeue_cpu_cycle / (double) total_dequeue_pkts_num;


    return bess::utils::Format(
            "Enqueue Status:\\n"
            "%.6lf cycles/pkt\\n"
            "Dequeue Status:\\n"
            "%.6lf pkts/batch\\n"
            "%.6lf cycles/pkt\\n"
            "packets num in queue: %llu\\n"
            "drop num: %llu\\n"
            "Queue_out Status:\\n"
            "%s:%hhu/%s",
            avg_enqueue_cycle_per_pkt,
            avg_dequeue_batch_size,
            avg_dequeue_cycle_per_pkt,
            n_packets_total,
            port_->queue_stats[PACKET_DIR_OUT][qid_].dropped,
            port_->name().c_str(), qid_,
            port_->port_builder()->class_name().c_str()
    );
}

void FlowBundler::DeInit()
{
    delete pool_;
    for (int i = 0; i < QUEUE_NUM; i++)
    {
        bucket_allocator[i].deallocate(bucket[i], bucket_num);
    }
    delete m_queue;
    delete pktgen;

    if (port_) {
        port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_OUT,
                         &qid_, 1);
    }

}

ADD_MODULE(FlowBundler, "FlowBundler", "FlowBundler traffic shaping")









