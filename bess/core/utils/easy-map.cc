//
// Created by admin on 2021/12/7.
//

#include "easy-map.h"
#include "flow_rate_estimator.h"


/// If flow_num is not an integer power of 2, set flow_num_
/// to a minimum power of 2 that is not less than flow_num
EasyMap::EasyMap(uint32_t flow_num)
{
    uint32_t MAX = 1 << 31;
    if (flow_num >= MAX)
        flow_num_ = MAX;
    else
    {
        uint32_t m = 1;
        while (m < flow_num)
            m *= 2;
        flow_num_ = m;
    }

    bucket = new EstimateInfo[flow_num_];
//    for (int i = 0; i < flow_num_; ++i)
//    {
//        bucket[i] = new EstimateInfo(20000000.0);
//    }

}

void EasyMap::Insert(uint32_t n, uint64_t rate)
{
    bucket[n].rate = rate;
}

EstimateInfo& EasyMap::GetEstimateInfo(uint32_t n)
{
    return bucket[n];
}

uint64_t EasyMap::Get(uint32_t key, uint32_t& n)
{
    n = do_hash(key) & (flow_num_ - 1);
    return bucket[n].rate;
}

// https://stackoverflow.com/questions/664014/what-integer-hash-function-are-good-that-accepts-an-integer-hash-key
uint32_t EasyMap::do_hash(uint32_t key)
{
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = (key >> 16) ^ key;

    return key;
}

void EasyMap::Reset()
{
    for (int i = 0; i < flow_num_; ++i)
    {
        bucket[i].Reset();
    }
}

EasyMap::~EasyMap() { delete[] bucket; }
