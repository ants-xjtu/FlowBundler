//
// Created by admin on 2022/1/6.
//

#ifndef HY_BATCHER_FLOW_RATE_ESTIMATOR_H
#define HY_BATCHER_FLOW_RATE_ESTIMATOR_H

#include "flow.h"
#include "circular-queue.h"
#include <string>
#include "../packet.h"

using bess::Packet;

class FlowRateEstimator
{
public:

    FlowRateEstimator(uint32_t interval) :
            estimate_interval(interval), a(0.25){}

    void UpdateRate(uint32_t pkt_bytes, uint64_t send_ts, uint32_t hash_id, uint64_t bucket_id);

//    void SetDstIP(uint32_t _dst_ip);


private:
    uint32_t estimate_interval;
    double a;
};

class EstimateInfo
{
public:
    friend class FlowRateEstimator;
    double rate;

    EstimateInfo() : count(0), total_bytes(0), max_ts(0), min_ts(UINT64_MAX), last_bucket(UINT32_MAX),
                     rate(20000000.0) {}

    EstimateInfo(double rate_) : count(0), total_bytes(0), max_ts(0), min_ts(UINT64_MAX), last_bucket(UINT32_MAX),
                                 rate(rate_) {}
    void Reset()
    {
        count = 0;
        total_bytes = 0;
        max_ts = 0;
        min_ts = UINT64_MAX;
        last_bucket = UINT32_MAX;
        rate = 20000000.0;
    }
private:
    uint64_t total_bytes;
    uint64_t max_ts;
    uint64_t min_ts;
    uint32_t last_bucket;
    uint32_t count;
};

extern FlowRateEstimator estimator;

#endif //HY_BATCHER_FLOW_RATE_ESTIMATOR_H
