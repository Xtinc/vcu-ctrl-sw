#include "QueueStatsCsvWriter.h"

#include <cstdio>
#include <iomanip>
#include <utility>

extern "C"
{
#include "lib_rtos/message.h"
}

namespace
{
constexpr auto RECORD_PERIOD = std::chrono::milliseconds(100);
constexpr auto IDLE_GAP = std::chrono::seconds(5);
constexpr const char *CSV_HEADER =
    "elapsed_ms,segment_id,idle_gap_ms,q_avg_fi_ms,q_fb_fi_ms,q_out_fi_ms,q_jitter_ms,q_jitter_frames,"
    "q_disorder_frames,q_max_disorder_depth,q_tail_jitter_ms,q_tail_jitter_frames,q_buffered_frames,"
    "q_adaptive_depth,q_depth_raw,q_depth_error_frames,q_pressure_frames,q_pacing_factor,q_recv_delta,"
    "q_dlv_delta,q_skip_delta,q_drop_delta,q_dup_delta,q_late_delta,q_reorder_delta,q_stale_delta,q_ovf_delta";

uint64_t delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : current;
}
} // namespace

QueueStatsCsvWriter::QueueStatsCsvWriter(std::string path, size_t max_file_bytes, size_t archive_count)
    : path_(std::move(path)), max_file_bytes_(max_file_bytes), archive_count_(archive_count)
{
}

QueueStatsCsvWriter::~QueueStatsCsvWriter()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close();
}

void QueueStatsCsvWriter::start(Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    close();
    bytes_written_ = 0;
    enabled_ = true;
    file_failed_ = false;
    has_last_frame_ = false;
    has_last_write_ = false;
    has_baseline_ = false;
    segment_id_ = 0;
    start_time_ = now;
    baseline_ = QueueStatsSnapshot{};
    clear_files();
}

void QueueStatsCsvWriter::on_frame(Clock::time_point now, const QueueStatsSnapshot &stats)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    double idle_gap_ms = 0.0;
    bool force_write = false;
    if (!has_last_frame_)
    {
        segment_id_ = 1;
        has_last_frame_ = true;
        force_write = true;
    }
    else
    {
        const auto frame_gap = now - last_frame_time_;
        if (frame_gap > IDLE_GAP)
        {
            ++segment_id_;
            idle_gap_ms = std::chrono::duration<double, std::milli>(frame_gap).count();
            force_write = true;
        }
    }
    last_frame_time_ = now;

    if (!force_write && has_last_write_ && now - last_write_time_ < RECORD_PERIOD)
        return;

    write(now, idle_gap_ms, stats);
}

void QueueStatsCsvWriter::stop(Clock::time_point now, const QueueStatsSnapshot &stats)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    if (has_last_frame_ && file_.is_open() && !file_failed_)
    {
        const auto frame_gap = now - last_frame_time_;
        double idle_gap_ms = 0.0;
        if (frame_gap > IDLE_GAP)
        {
            ++segment_id_;
            idle_gap_ms = std::chrono::duration<double, std::milli>(frame_gap).count();
        }
        write(now, idle_gap_ms, stats);
    }

    close();
    enabled_ = false;
}

void QueueStatsCsvWriter::write(Clock::time_point now, double idle_gap_ms, const QueueStatsSnapshot &stats)
{
    if (file_failed_)
        return;

    if (file_.is_open() && max_file_bytes_ > 0 && bytes_written_ >= max_file_bytes_)
    {
        rotate();
        if (!open())
            return;
    }
    else if (!file_.is_open() && !open())
    {
        return;
    }

    const QueueStatsSnapshot previous = has_baseline_ ? baseline_ : QueueStatsSnapshot{};
    const double elapsed = std::chrono::duration<double, std::milli>(now - start_time_).count();

    file_ << std::fixed << std::setprecision(3) << elapsed << ',' << segment_id_ << ',' << idle_gap_ms << ','
          << std::setprecision(2) << stats.avg_frame_interval_ms << ',' << stats.feedback_interval_ms << ','
          << stats.output_interval_ms << ',' << stats.jitter_ms << ',' << stats.jitter_frames << ','
          << stats.disorder_frames << ',' << stats.max_disorder_depth << ',' << stats.tail_jitter_ms << ','
          << stats.tail_jitter_frames << ',' << stats.buffered_frames << ',' << stats.adaptive_depth << ','
          << stats.raw_depth_frames << ',' << stats.depth_error_frames << ',' << stats.pressure_frames << ','
          << stats.pacing_factor << ',' << delta(stats.recv, previous.recv) << ','
          << delta(stats.deliver, previous.deliver) << ',' << delta(stats.skip, previous.skip) << ','
          << delta(stats.drop, previous.drop) << ',' << delta(stats.duplicate, previous.duplicate) << ','
          << delta(stats.late, previous.late) << ',' << delta(stats.reorder, previous.reorder) << ','
          << delta(stats.stale, previous.stale) << ',' << delta(stats.overflow, previous.overflow) << '\n';
    if (!file_)
    {
        file_failed_ = true;
        VIDEO_ERROR_PRINT("Failed to write %s", path_.c_str());
        return;
    }

    const auto position = file_.tellp();
    if (position != std::streampos(-1))
        bytes_written_ = static_cast<size_t>(position);
    baseline_ = stats;
    has_baseline_ = true;
    last_write_time_ = now;
    has_last_write_ = true;
}

bool QueueStatsCsvWriter::open()
{
    file_.open(path_, std::ios::out | std::ios::trunc);
    if (!file_)
    {
        file_failed_ = true;
        VIDEO_ERROR_PRINT("Failed to open %s", path_.c_str());
        return false;
    }

    file_ << CSV_HEADER << '\n';
    if (!file_)
    {
        file_failed_ = true;
        VIDEO_ERROR_PRINT("Failed to write header to %s", path_.c_str());
        return false;
    }
    bytes_written_ = std::char_traits<char>::length(CSV_HEADER) + 1;
    return true;
}

void QueueStatsCsvWriter::clear_files()
{
    close();
    std::remove(path_.c_str());
    for (size_t index = 1; index <= archive_count_; ++index)
        std::remove(archive_path(index).c_str());
}

void QueueStatsCsvWriter::rotate()
{
    close();
    if (archive_count_ == 0)
    {
        std::remove(path_.c_str());
        return;
    }

    std::remove(archive_path(archive_count_).c_str());
    for (size_t index = archive_count_; index > 1; --index)
        std::rename(archive_path(index - 1).c_str(), archive_path(index).c_str());
    std::rename(path_.c_str(), archive_path(1).c_str());
}

void QueueStatsCsvWriter::close()
{
    if (!file_.is_open())
        return;
    file_.flush();
    file_.close();
}

std::string QueueStatsCsvWriter::archive_path(size_t index) const
{
    return path_ + "." + std::to_string(index);
}
