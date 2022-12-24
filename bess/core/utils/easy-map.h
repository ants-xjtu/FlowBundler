//
// Created by admin on 2021/12/7.
//

#ifndef HY_BATCHER_EASY_MAP_H
#define HY_BATCHER_EASY_MAP_H

#include <iostream>

///
///this map only used for aggregation rate
///

class EstimateInfo;

class EasyMap
{
public:
    EasyMap(uint32_t flow_num);

    void Insert(uint32_t n, uint64_t value);

    EstimateInfo& GetEstimateInfo(uint32_t n);

    uint64_t Get(uint32_t key, uint32_t& n);

    void Reset();

    ~EasyMap();

private:
    uint32_t flow_num_;
    EstimateInfo* bucket;

    uint32_t do_hash(uint32_t key);
};


#endif //HY_BATCHER_EASY_MAP_H
