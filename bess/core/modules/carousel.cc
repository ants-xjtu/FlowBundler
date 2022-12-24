//
// Created by admin on 2021/10/8.
//

#include "carousel.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Tcp;

const Commands Carousel::cmds = {
        {"set_max_dequeue_burst", "CarouselMaxDequeueBurstArg", MODULE_CMD_FUNC(
                &Carousel::SetMaxDequeueBurst), Command::THREAD_SAFE},
        {"set_default_rate",      "CarouselDefaultRateArg",     MODULE_CMD_FUNC(
                &Carousel::SetDefaultRate),     Command::THREAD_SAFE},
        {"add_flow",              "CarouselCommandAddArg",      MODULE_CMD_FUNC(
                &Carousel::AddFlow),            Command::THREAD_SAFE},
        {"set_generate_burst",    "CarouselGenerateBurstArg",   MODULE_CMD_FUNC(
                &Carousel::SetGenerateBurst),   Command::THREAD_SAFE},
        {"set_generate_pkt_size", "CarouselGeneratePktSizeArg", MODULE_CMD_FUNC(
                &Carousel::SetGeneratePktSize), Command::THREAD_SAFE}
};

CommandResponse Carousel::Init(const bess::pb::CarouselArg &arg)
{
    task_id_t tid;
    tid = RegisterTask(nullptr);
    if (tid == INVALID_TASK_ID)
    {
        return CommandFailure(ENOMEM, "task creation failed");
    }
    // init mempool
    uint32_t mempool_size = arg.size();
    pool_ = new MemoryPool<PacketNode>(mempool_size, "carousel");

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

    //init packet queue
    queue_ = new std::vector<pkts_bucket *>;
    queue_->resize(slots, nullptr);
    bucket = bucket_allocator.allocate(slots);
    for (uint64_t i = 0; i < slots; ++i)
    {
        (*queue_)[i] = bucket + i;
        bucket_allocator.construct(bucket + i, pool_, MAX_BYTES_PER_FLOW);
    }

    header_ts = tsc_to_ns(rdtsc()) / granularity_;

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


CommandResponse Carousel::AddFlow(const bess::pb::CarouselCommandAddArg &arg)
{
    uint32_t flow_num = arg.flows_size() > 0 ? arg.flows_size() : arg.flow_num();
    uint64_t now = tsc_to_ns(rdtsc());
    flow_info.resize(flow_num);
    pkts_bucket::pkts_num.resize(flow_num, 0);
    pkts_bucket::flow_to_gen.Reset();

    LOG(INFO) << "=============Add Flow================" << std::endl;
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

CommandResponse Carousel::SetDefaultRate(const bess::pb::CarouselDefaultRateArg &arg)
{
    return CommandSuccess();
}

CommandResponse Carousel::SetMaxDequeueBurst(const bess::pb::CarouselMaxDequeueBurstArg &arg)
{
    max_dequeue_burst_ =
            arg.max_dequeue_burst() > (int) bess::PacketBatch::kMaxBurst ? (int) bess::PacketBatch::kMaxBurst
                                                                         : arg.max_dequeue_burst();
    return CommandSuccess();
}

CommandResponse Carousel::SetGenerateBurst(const bess::pb::CarouselGenerateBurstArg &arg)
{
    pktgen->SetGenerateBurst(arg.generate_burst());
    return CommandSuccess();
}

CommandResponse Carousel::SetGeneratePktSize(const bess::pb::CarouselGeneratePktSizeArg &arg)
{
    pktgen->SetGenerateSize(arg.generate_pkt_size());
    return CommandSuccess();
}


struct task_result Carousel::RunTask(Context *ctx, bess::PacketBatch *batch, void *)
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
#if MULTI_BUCKET_DEQUEUE == 0
    Dequeue(batch);
#else
    //    MultiBucketDequeue(batch);
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
                dequeue_state.cpu_cycle += dequeue_end_cycle - dequeue_begin_cycle;
                dequeue_state.pkts_num += sent_pkts;
                dequeued_batch ++;
            }
            packets_num-=sent_pkts;
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

#if CNT_DEBUG >= 1
    LOG(INFO)<<"dequeue cnt: "<<cnt;
    LOG(INFO)<<"head: "<<header_ts % slots;
#endif

        const int pkt_overhead = 24;

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

int Carousel::Insert(bess::Packet *packet, uint64_t &send_time, uint32_t flow_id)
{
    uint64_t index;
    uint64_t grn = send_time / granularity_;
#if CAROUSEL_ENQUEUE_DEBUG >= 1
    LOG(INFO)<<"send_time "<<send_time;
    LOG(INFO)<<"grn "<<grn<<"  header_ts "<<header_ts;
#endif
    if (grn > header_ts + slots - 1)
    {
        return -1;
    } else if (grn < header_ts)
    {
        grn = header_ts;
        send_time = grn * granularity_;
    }
    index = grn % slots;

#if CAROUSEL_ENQUEUE_DEBUG >= 1
    LOG(INFO)<<"index: "<<index;
#endif

    int enqueue_num = (*queue_)[index]->enqueue_packet(packet, flow_id);
    return enqueue_num;
}

void Carousel::Enqueue(Context *ctx, bess::PacketBatch *batch)
{
    int cnt = batch->cnt();
    if (cnt < 1)
    {
        return;
    }
#if CAROUSEL_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "===============Enqueue begin===============";
    LOG(INFO) << "cnt: "<<cnt;
#endif
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
#if CAROUSEL_ENQUEUE_DEBUG >= 1
    LOG(INFO) << "now "<<now;
#endif
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

        if (packets_num == 0)
            header_ts = now/granularity_;
        new_stamp = new_stamp >= now ? new_stamp : now;
        enqueue_result = Insert(pkt, new_stamp, flow_id);
        if (enqueue_result < 0)
        {
            DropPacket(ctx, pkt);
        } else
        {
            info.last_stamp = new_stamp;
            packets_num++;
            enqueue_state.pkts_num ++;
        }
#if CAROUSEL_ENQUEUE_DEBUG >= 1
        LOG(INFO) << "lst_stamp = " << lst_stamp << ", real_send_stamp = " << new_stamp;
#endif
    }
    enqueue_end_cycle = rdtsc();
    enqueue_state.cpu_cycle += enqueue_end_cycle - enqueue_begin_cycle;
}

void Carousel::Dequeue(bess::PacketBatch *batch)
{
#if CAROUSEL_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "===============dequeue begin===============";
#endif
    uint64_t now;
    uint64_t index;
    int dequeued = 0;
    int quota = ACCESS_ONCE(max_dequeue_burst_);

    batch->clear();
    dequeue_burst = 0;
    now = tsc_to_ns(rdtsc()) / granularity_;
#if CAROUSEL_DEQUEUE_DEBUG >= 1
    LOG(INFO) << "now: "<<now<<"  header:"<<header_ts;
    LOG(INFO) << "quota: "<<quota;
#endif
    while (now >= header_ts and quota > 0)
    {
        index = header_ts % slots;
        if ((*queue_)[index]->IsEmpty())
        {
#if CAROUSEL_DEQUEUE_DEBUG >= 1
            LOG(INFO) << "skip this empty bucket";
#endif
            header_ts++;
            continue;
        }
        dequeued = (*queue_)[index]->dequeue_packet_bulk(batch, quota);
#if CAROUSEL_DEQUEUE_DEBUG >= 1
        LOG(INFO) << "index: "<<index<<"  dequeue num:"<< dequeued<<"  quota: "<<quota;
        LOG(INFO) << "after dequeue packets num" << (*queue_)[index]->GetNodeNum();
#endif
        if ((*queue_)[index]->IsEmpty())
        {
            header_ts++;
        }
        break;
    }
    dequeue_burst = dequeued;
    batch->set_cnt(dequeued);
}

std::string Carousel::GetDesc() const
{
    double avg_cycle_enqueue_pkt = (double) enqueue_state.cpu_cycle / (double) enqueue_state.pkts_num;
    double avg_cycle_dequeue_pkt = (double) dequeue_state.cpu_cycle / (double) dequeue_state.pkts_num;
    double avg_dequeue_size = (double)dequeue_state.pkts_num / ACCESS_ONCE(dequeued_batch);
    uint64_t packets = ACCESS_ONCE(packets_num);
    return bess::utils::Format(
            "enqueue status:\\n"
            "avg cpu cycle consume per pkt: %.6lf\\n"
            "total enqueued pkts number: %llu\\n"
            "dequeue status:\\n"
            "avg cpu cycle consume per pkt: %.6lf\\n"
            "total dequeued pkts number: %llu\\n"
            "avg dequeue size: %.6lf\\n"
            "packets num in queue: %llu\\n"
            "drop num: %llu\\n"
            "queue_out Status:\\n"
            "%s:%hhu/%s",
            avg_cycle_enqueue_pkt,
            enqueue_state.pkts_num,
            avg_cycle_dequeue_pkt,
            dequeue_state.pkts_num,
            avg_dequeue_size,
            packets,
            port_->queue_stats[PACKET_DIR_OUT][qid_].dropped,
            port_->name().c_str(), qid_,
            port_->port_builder()->class_name().c_str()
    );
}

void Carousel::DeInit()
{
    delete pool_;
    bucket_allocator.deallocate(bucket, slots);
    delete queue_;
    delete pktgen;

    if (port_) {
        port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_OUT,
                         &qid_, 1);
    }
}


ADD_MODULE(Carousel, "carousel", "another traffic shaping")
