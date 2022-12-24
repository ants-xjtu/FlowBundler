//
// Created by admin on 2022/1/6.
//

#include "flow_rate_estimator.h"
#include "hb-list-bucket.h"
void FlowRateEstimator::UpdateRate(uint32_t pkt_bytes, uint64_t send_ts, uint32_t hash_id, uint64_t bucket_id)
{
    EstimateInfo& info = HBListBucket::aggregation_rates.GetEstimateInfo(hash_id);

    if (info.last_bucket== bucket_id)
    {
        info.total_bytes += pkt_bytes;
        return;
    }

    info.last_bucket = bucket_id;
    info.count++;

    info.max_ts = send_ts > info.max_ts ? send_ts : info.max_ts;
    info.min_ts = send_ts < info.min_ts ? send_ts : info.min_ts;

    if (info.count == estimate_interval + 1)
    {
        double loss_rate = (double) (info.total_bytes << 3) / (info.max_ts - info.min_ts);
        info.rate = info.rate * (1 - a) + a * loss_rate*1000000;

        info.count = 1;
        info.total_bytes = 0;
        info.max_ts = send_ts;
        info.min_ts = send_ts;
    }
    info.total_bytes += pkt_bytes;
}


//void FlowRateEstimator::SetDstIP(uint32_t _dst_ip)
//{
//    auto estimate = data.find(_dst_ip);
//    if (estimate != data.end())
//        return;
//    HBListBucket::aggregation_rates.Insert(_dst_ip, 2000);
//    data[_dst_ip] = Estimate(0);
//}
FlowRateEstimator estimator(5);


