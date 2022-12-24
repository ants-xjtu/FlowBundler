//
// Created by admin on 2021/10/29.
//

#include "list-bucket.h"

std::vector<uint64_t> ListBucket::pkts_num = {};
CircularQueue<uint32_t> ListBucket::flow_to_gen = CircularQueue<uint32_t>(8192*256);
