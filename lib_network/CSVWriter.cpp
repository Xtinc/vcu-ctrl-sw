#include "CSVWriter.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
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
    "timestamp_utc,session_id,segment_id,idle_gap_s,q_short_fi_ms,q_avg_fi_ms,q_out_fi_ms,q_jitter_ms,"
    "q_disorder_frames,q_max_disorder_depth,q_tail_jitter_ms,q_buffered_frames,q_adaptive_depth,q_depth_raw,"
    "q_pressure_frames,q_recv_delta,"
    "q_dlv_delta,q_skip_delta,q_drop_delta,q_dup_delta,q_late_delta,q_reorder_delta,q_stale_delta,q_ovf_delta,"
    "allow_immediate";

uint64_t delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : current;
}
} // namespace

NetCSVWriter::NetCSVWriter(std::string path, size_t max_file_bytes, size_t archive_count)
    : path_(std::move(path)), max_file_bytes_(max_file_bytes), archive_count_(archive_count)
{
}

NetCSVWriter::~NetCSVWriter()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close();
}

void NetCSVWriter::start(Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    close();
    bytes_written_ = 0;
    enabled_ = true;
    file_failed_ = false;
    has_last_frame_ = false;
    has_last_write_ = false;
    write_pending_ = false;
    segment_id_ = 0;
    start_time_ = now;
    wall_start_time_ = WallClock::now();
    session_id_ = std::chrono::duration_cast<std::chrono::microseconds>(wall_start_time_.time_since_epoch()).count();
    baseline_ = QueueStatsSnapshot{};
    if (active_file_exists())
        rotate();
}

bool NetCSVWriter::on_frame(Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return false;

    double idle_gap_s = 0.0;
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
            idle_gap_s = std::chrono::duration<double>(frame_gap).count();
            force_write = true;
        }
    }
    last_frame_time_ = now;

    if (!force_write && has_last_write_ && now - last_write_time_ < RECORD_PERIOD)
        return false;

    pending_time_ = now;
    pending_idle_gap_s_ = idle_gap_s;
    write_pending_ = true;
    return true;
}

void NetCSVWriter::write(const QueueStatsSnapshot &stats)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !write_pending_)
        return;

    write_pending_ = false;
    write_row(pending_time_, pending_idle_gap_s_, stats);
}

void NetCSVWriter::stop(Clock::time_point now, const QueueStatsSnapshot &stats)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
        return;

    write_pending_ = false;
    if (has_last_frame_ && !file_failed_)
        write_row(now, 0.0, stats);

    close();
    enabled_ = false;
}

void NetCSVWriter::write_row(Clock::time_point now, double idle_gap_s, const QueueStatsSnapshot &stats)
{
    if (file_failed_)
        return;

    if (file_.is_open() && max_file_bytes_ > 0 && bytes_written_ >= max_file_bytes_)
    {
        if (!rotate())
            return;
        if (!open())
            return;
    }
    else if (!file_.is_open() && !open())
    {
        return;
    }

    const QueueStatsSnapshot previous = baseline_;
    file_ << timestamp_utc(now) << ',' << session_id_ << ',' << segment_id_ << ',' << std::fixed << std::setprecision(3)
          << idle_gap_s << ',' << std::setprecision(2) << stats.fi_short_ms << ',' << stats.fi_avg_ms << ','
          << stats.fi_out_ms << ',' << stats.jitter_ms << ',' << stats.disorder_fr << ',' << stats.disorder_max_fr
          << ',' << stats.jitter_tail_ms << ',' << stats.buf_fr << ',' << stats.depth_target_fr << ','
          << stats.depth_raw_fr << ',' << stats.pressure_fr << ',' << delta(stats.recv, previous.recv) << ','
          << delta(stats.dlv, previous.dlv) << ',' << delta(stats.skip, previous.skip) << ','
          << delta(stats.drop, previous.drop) << ',' << delta(stats.dup, previous.dup) << ','
          << delta(stats.late, previous.late) << ',' << delta(stats.reorder, previous.reorder) << ','
          << delta(stats.stale, previous.stale) << ',' << delta(stats.ovf, previous.ovf) << ','
          << (stats.allow_immediate ? 1 : 0) << '\n';
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
    last_write_time_ = now;
    has_last_write_ = true;
}

bool NetCSVWriter::open()
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

bool NetCSVWriter::active_file_exists() const
{
    std::ifstream input(path_);
    return input.good();
}

bool NetCSVWriter::rotate()
{
    close();
    if (archive_count_ == 0)
    {
        if (std::remove(path_.c_str()) != 0 && errno != ENOENT)
        {
            file_failed_ = true;
            VIDEO_ERROR_PRINT("Failed to remove %s during rotation: %s", path_.c_str(), std::strerror(errno));
            return false;
        }
        return true;
    }

    const auto oldest = archive_path(archive_count_);
    if (std::remove(oldest.c_str()) != 0 && errno != ENOENT)
    {
        file_failed_ = true;
        VIDEO_ERROR_PRINT("Failed to remove %s during rotation: %s", oldest.c_str(), std::strerror(errno));
        return false;
    }

    for (size_t index = archive_count_; index > 1; --index)
    {
        const auto source = archive_path(index - 1);
        const auto destination = archive_path(index);
        if (std::rename(source.c_str(), destination.c_str()) != 0 && errno != ENOENT)
        {
            file_failed_ = true;
            VIDEO_ERROR_PRINT("Failed to rotate %s to %s: %s", source.c_str(), destination.c_str(),
                              std::strerror(errno));
            return false;
        }
    }

    const auto first_archive = archive_path(1);
    if (std::rename(path_.c_str(), first_archive.c_str()) != 0)
    {
        file_failed_ = true;
        VIDEO_ERROR_PRINT("Failed to rotate %s to %s: %s", path_.c_str(), first_archive.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

void NetCSVWriter::close()
{
    if (!file_.is_open())
        return;
    file_.close();
}

std::string NetCSVWriter::archive_path(size_t index) const
{
    return path_ + "." + std::to_string(index);
}

std::string NetCSVWriter::timestamp_utc(Clock::time_point now) const
{
    const auto wall_time = wall_start_time_ + (now - start_time_);
    const auto epoch_us = std::chrono::duration_cast<std::chrono::microseconds>(wall_time.time_since_epoch()).count();
    const std::time_t seconds = static_cast<std::time_t>(epoch_us / 1000000);
    const auto micros = static_cast<unsigned long long>(epoch_us % 1000000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(6) << micros << 'Z';
    return out.str();
}
