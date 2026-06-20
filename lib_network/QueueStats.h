#ifndef QUEUE_STATS_H
#define QUEUE_STATS_H

#include <cstdint>

struct QueueStatsSnapshot
{
    double short_frame_interval_ms = 0.0;
    double avg_frame_interval_ms = 0.0;
    double output_interval_ms = 0.0;
    double jitter_ms = 0.0;
    double tail_jitter_ms = 0.0;
    double disorder_frames = 0.0;
    uint64_t max_disorder_depth = 0;
    uint64_t buffered_frames = 0;
    uint64_t adaptive_depth = 0;
    double raw_depth_frames = 0.0;
    uint64_t pressure_frames = 0;
    uint64_t recv = 0;
    uint64_t deliver = 0;
    uint64_t skip = 0;
    uint64_t drop = 0;
    uint64_t duplicate = 0;
    uint64_t late = 0;
    uint64_t reorder = 0;
    uint64_t stale = 0;
    uint64_t overflow = 0;
};

#endif // QUEUE_STATS_H
