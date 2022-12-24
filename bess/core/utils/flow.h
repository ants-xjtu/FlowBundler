//
// Created by admin on 2021/10/15.
//

#ifndef HY_BATCHER_FLOW_H
#define HY_BATCHER_FLOW_H
#include "ether.h"
#include "ip.h"
#include "udp.h"
#include "tcp.h"
#include "format.h"
#include "time.h"
#include <glog/logging.h>


#define NSEC_PER_SEC 1000000000ull;
#define NSEC_PER_USEC 1000ull;
#define USEC_PER_SEC 1000000ull;

#define DEFAULT_FLOW_RATE 200ull;

/*
 * when transforming from Mbits/s to bits/ns, just multiply 0.001 to
 * Get an approximate result. Namely, when calculating the send_timestamp, just
 * multiply 1000
 * */
#define RATE_SCALE 1000000ull

//gcc provides a function to find first set
#define _ffs(a) __builtin_ffsll(a)

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::be32_t;
using bess::utils::be16_t;

using bit_map_layer = std::vector<uint64_t>;

struct FlowInfo
{
    uint64_t last_stamp; // ns
    uint64_t rate; // Kbits/s
};

struct FourTuple
{
    be32_t src_ip;
    be32_t dst_ip;
    be16_t src_port;
    be16_t dst_port;
};

/*!
 * multi-level bit map, which could construct itself according
 * to the slots.
 */
struct BitMap
{
    int n_layers;
    std::vector<bit_map_layer> bit_map;

    explicit BitMap(uint64_t slots)
    {
        n_layers = 0;
        uint64_t tmp = 1;

        while (slots > tmp)
        {
            tmp = tmp * 64;
            n_layers++;
        }
        if (slots == 1)
            n_layers++;

        bit_map.resize(n_layers);
        for (int i = 0; i < n_layers; ++i)
        {
            bit_map[i].resize(1ull << (i * 6), 0);
        }
    }
};

/*!
 * this function is used to set bit to 1 for every layers' bitmap when the packet
 * number of one queue's bucket changes from zero to non-zero. The function "FreeBitMap"
 * does an opposite action.
 * @param index
 * @param map
 */
static inline void SetBitMap(uint64_t index, BitMap *map)
{
    int n_layers = map->n_layers;
    uint64_t layer_seq = index;
    uint16_t seq;
    uint16_t tail;

    /// if the uint64 set in the last layer not equals zero, we don't need to
    /// change all the bit map, just adjusting the certain bit in this uint64 set
    /// to 1 using the operation "or"

    for (int i = n_layers - 1; i >= 0; --i)
    {
        tail = (layer_seq + 1) & 0b111111;
        layer_seq = tail == 0 ? ((layer_seq + 1) >> 6) - 1 : ((layer_seq + 1) >> 6);
        seq = tail == 0 ? 63 : tail - 1;

        if (map->bit_map[i][layer_seq] == 0)
            map->bit_map[i][layer_seq] = (map->bit_map[i][layer_seq]) | (1ull << seq);
        else
        {
            map->bit_map[i][layer_seq] = (map->bit_map[i][layer_seq]) | (1ull << seq);
            break;
        }
    }
}

static inline void FreeBitMap(uint64_t index, BitMap *map)
{
    int n_layers = map->n_layers;
    uint64_t layer_seq = index;
    uint16_t seq;
    uint16_t tail ;

    for (int i = n_layers - 1; i >= 0; --i)
    {
        tail = (layer_seq + 1) & 0b111111;
        layer_seq = tail == 0 ? ((layer_seq + 1) >> 6) - 1 : ((layer_seq + 1) >> 6);
        seq = tail == 0 ? 63 : tail - 1;

        map->bit_map[i][layer_seq] &= ~(1ull << seq);
        if (map->bit_map[i][layer_seq] != 0)
            return;
    }
}

static inline uint32_t HashKey(FourTuple &tuple)
{
    return (tuple.src_ip.value() * 59) ^ tuple.dst_ip.value() ^ (static_cast<uint32_t>(tuple.src_port.value()) << 16) ^
           static_cast<uint32_t>(tuple.dst_port.value());
}

static inline uint32_t HashKey(be32_t src_ip, be32_t dst_ip, be16_t src_port, be16_t dst_port)
{
    return (src_ip.value() * 59) ^ dst_ip.value() ^ (static_cast<uint32_t>(src_port.value()) << 16) ^
           static_cast<uint32_t>(dst_port.value());
}
#endif //HY_BATCHER_FLOW_H
