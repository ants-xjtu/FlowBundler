//
// Created by admin on 2021/10/28.
//

#ifndef HY_BATCHER_PACKETGENERATOR_H
#define HY_BATCHER_PACKETGENERATOR_H

#include "flow.h"
#include <string>
#include <vector>
#include "../worker.h"
#include "../pktbatch.h"
#include "../packet.h"
#include "../packet_pool.h"
#include "hb-list-bucket.h"
#include "list-bucket.h"
#include "../utils/checksum.h"
#include "../utils/random.h"

#define UINT32_MAX  ((uint32_t)-1)

using bess::utils::be32_t;
using bess::utils::be16_t;

static const size_t kMaxTemplateSize = 1536;
struct PacketTemplate
{
    uint16_t size_;
    unsigned char template_[kMaxTemplateSize];
};

class PacketGenerator
{
    using pkts_bucket = HBListBucket;

public:
    PacketGenerator(uint32_t generate_burst, std::string templates, size_t n, uint32_t flows_per_dst)
            : generate_burst_(generate_burst), num_templates_(n), cur_pkt_template(0), flows_per_dst_(flows_per_dst)
    {
        random.SetSeed(0xBAADF00DDEADBEEFul);
        uint32_t rand_dst;
        uint64_t template_length;
        if (templates.length() > kMaxTemplateSize)
            LOG(INFO) << "template is too big";
        templates_.resize(n);

        // Generate a packet
        bess::Packet *pkt;

        if (!(pkt = current_worker.packet_pool()->Alloc())) {
            return;
        }

        char *p = pkt->buffer<char *>() + SNBUF_HEADROOM;
        Ethernet *eth = reinterpret_cast<Ethernet *>(p);
        Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

        pkt->set_data_off(SNBUF_HEADROOM);
        pkt->set_total_len(generate_pkt_size_);
        pkt->set_data_len(generate_pkt_size_);

        for (uint32_t i = 0; i < n / flows_per_dst_; ++i)
        {
            rand_dst = random.GetRange(UINT32_MAX);
            for (uint32_t j = 0; j < flows_per_dst_; j++)
            {
                template_length = templates.length() > kMaxTemplateSize ? kMaxTemplateSize : templates.length();

                bess::utils::Copy(p, templates.c_str(), template_length, true);

                ip->src = be32_t(i * flows_per_dst_ + j);
                ip->dst = be32_t(rand_dst);

                size_t ip_bytes = (ip->header_length) << 2;
                Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<char *>(ip) + ip_bytes);
                udp->src_port = be16_t(1);
                udp->dst_port = be16_t(1);

                udp->checksum = bess::utils::CalculateIpv4UdpChecksum(*ip, *udp);
                ip->checksum = bess::utils::CalculateIpv4Checksum(*ip);

                memset(templates_[i * flows_per_dst_ + j].template_, 0, kMaxTemplateSize);
                bess::utils::Copy(templates_[i * flows_per_dst_ + j].template_, p, template_length);
                templates_[i * flows_per_dst_ + j].size_ = template_length;
            }
        }
        bess::Packet::Free(pkt);
    }

    bool AllocPackets(bess::PacketBatch *batch);

    void RewriteUsingSingleTemplate(bess::PacketBatch *batch);

    void SetGenerateBurst(uint32_t burst) { generate_burst_ = burst; }

    void SetGenerateSize(uint32_t size) { generate_pkt_size_ = size; }

    uint32_t GenerateBurst() { return generate_burst_; }


private:
    uint32_t generate_burst_ = 32;
    uint32_t generate_pkt_size_ = 1500;
    uint64_t num_templates_;
    std::vector<PacketTemplate> templates_;
    uint32_t flows_per_dst_;
    uint32_t cur_pkt_template; // Index of current used packet index
    Random random;
};


#endif //HY_BATCHER_PACKETGENERATOR_H
