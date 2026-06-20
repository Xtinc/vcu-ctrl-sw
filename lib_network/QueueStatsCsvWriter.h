#ifndef QUEUE_STATS_CSV_WRITER_H
#define QUEUE_STATS_CSV_WRITER_H

#include "QueueStats.h"
#include <chrono>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

class QueueStatsCsvWriter
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit QueueStatsCsvWriter(std::string path = "queue_stats.csv", size_t max_file_bytes = 64u * 1024u * 1024u,
                                 size_t archive_count = 3);
    ~QueueStatsCsvWriter();

    void start(Clock::time_point now);
    void on_frame(Clock::time_point now, const QueueStatsSnapshot &stats);
    void stop(Clock::time_point now, const QueueStatsSnapshot &stats);

  private:
    void write(Clock::time_point now, double idle_gap_ms, const QueueStatsSnapshot &stats);
    bool open();
    void rotate();
    void clear_files();
    void close();
    std::string archive_path(size_t index) const;

    const std::string path_;
    const size_t max_file_bytes_;
    const size_t archive_count_;
    std::mutex mutex_;
    std::ofstream file_;
    size_t bytes_written_ = 0;
    bool enabled_ = false;
    bool file_failed_ = false;
    bool has_last_frame_ = false;
    bool has_last_write_ = false;
    bool has_baseline_ = false;
    uint64_t segment_id_ = 0;
    Clock::time_point start_time_{};
    Clock::time_point last_frame_time_{};
    Clock::time_point last_write_time_{};
    QueueStatsSnapshot baseline_;
};

#endif // QUEUE_STATS_CSV_WRITER_H
