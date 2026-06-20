#ifndef QUEUE_STATS_H
#define QUEUE_STATS_H

#include <cstdint>

struct QueueStatsSnapshot
{
    double fi_short_ms = 0.0;
    double fi_avg_ms = 0.0;
    double fi_out_ms = 0.0;
    double jitter_ms = 0.0;
    double jitter_tail_ms = 0.0;
    double disorder_fr = 0.0;
    uint64_t disorder_max_fr = 0;
    uint64_t buf_fr = 0;
    uint64_t depth_target_fr = 0;
    double depth_raw_fr = 0.0;
    uint64_t pressure_fr = 0;
    uint64_t recv = 0;
    uint64_t dlv = 0;
    uint64_t skip = 0;
    uint64_t drop = 0;
    uint64_t dup = 0;
    uint64_t late = 0;
    uint64_t reorder = 0;
    uint64_t stale = 0;
    uint64_t ovf = 0;
};

#endif // QUEUE_STATS_H
