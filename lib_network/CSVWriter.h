#ifndef CSV_WRITER_H
#define CSV_WRITER_H

#include "QueueStats.h"
#include <chrono>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

class NetCSVWriter
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit NetCSVWriter(std::string path = "queue_stats.csv", size_t max_file_bytes = 64u * 1024u * 1024u,
                          size_t archive_count = 3);
    ~NetCSVWriter();

    void start(Clock::time_point now);
    bool on_frame(Clock::time_point now);
    void write(const QueueStatsSnapshot &stats);
    void stop(Clock::time_point now, const QueueStatsSnapshot &stats);

  private:
    void write_row(Clock::time_point now, double idle_gap_s, const QueueStatsSnapshot &stats);
    bool open();
    bool rotate();
    bool active_file_exists() const;
    void close();
    std::string archive_path(size_t index) const;
    std::string timestamp_utc(Clock::time_point now) const;

    using WallClock = std::chrono::system_clock;

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
    bool write_pending_ = false;
    int64_t session_id_ = 0;
    uint64_t segment_id_ = 0;
    Clock::time_point start_time_{};
    WallClock::time_point wall_start_time_{};
    Clock::time_point last_frame_time_{};
    Clock::time_point last_write_time_{};
    Clock::time_point pending_time_{};
    double pending_idle_gap_s_ = 0.0;
    QueueStatsSnapshot baseline_;
};

#endif // CSV_WRITER_H
