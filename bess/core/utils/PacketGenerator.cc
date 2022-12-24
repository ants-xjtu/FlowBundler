//
// Created by admin on 2021/10/28.
//

#include "PacketGenerator.h"

bool PacketGenerator::AllocPackets(bess::PacketBatch *batch)
{
    batch->clear();
    uint32_t generate_num = pkts_bucket::flow_to_gen.ElementsNum() > generate_burst_ ? generate_burst_ : 
	    								pkts_bucket::flow_to_gen.ElementsNum();
    if (current_worker.packet_pool()->AllocBulk(batch->pkts(), generate_num, generate_pkt_size_))
    {
        batch->set_cnt(generate_num);
        return true;
    } else
    {
        return false;
    }
}

void PacketGenerator::RewriteUsingSingleTemplate(bess::PacketBatch *batch)
{
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++)
    {
	    int pos = pkts_bucket::flow_to_gen.Pop();
	    uint16_t size = templates_[pos].size_;
        const void *tpl = templates_[pos].template_;

        bess::Packet *pkt = batch->pkts()[i];
        char *ptr = pkt->buffer<char *>() + SNBUF_HEADROOM;

        pkt->set_data_off(SNBUF_HEADROOM);
        pkt->set_total_len(generate_pkt_size_);
        pkt->set_data_len(generate_pkt_size_);
	
	//memset((char *)pkt->data(), 0, generate_pkt_size_ - size);
        bess::utils::CopyInlined(ptr, tpl, size, true);
    }
}


