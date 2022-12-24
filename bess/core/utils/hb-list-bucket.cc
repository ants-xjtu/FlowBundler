//
// Created by admin on 2021/11/29.
//

#include "hb-list-bucket.h"

std::vector<uint64_t> HBListBucket::pkts_num = {};
// easy map
EasyMap HBListBucket::aggregation_rates = EasyMap(512);
CircularQueue<uint32_t> HBListBucket::flow_to_gen = CircularQueue<uint32_t>(8192*256);
