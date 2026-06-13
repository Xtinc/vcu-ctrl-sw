#include "lib_network/ReliableUDP.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr uint32_t TEST_MAGIC = 0x524a5443u; // RJTC

#pragma pack(push, 1)
struct JitterMsgHdr
{
    uint32_t magic;
    uint32_t seq;
    uint64_t send_elapsed_us;
    uint32_t frame_size;
    uint32_t payload_crc;
};
#pragma pack(pop)

struct Config
{
    uint32_t duration_sec = 30;
    double rate_mbps = 5.0;
    size_t payload_bytes = 1200;
    uint16_t sender_port = 15301;
    uint16_t receiver_port = 15302;
    uint32_t stats_period_ms = 100;
    std::string out_dir = "reliable_udp_jitter_out";
};

struct ParsedQueueStats
{
    double q_avg_fi_ms = 0.0;
    double q_jitter_ms = 0.0;
    double q_jitter_frames = 0.0;
    double q_tail_jitter_ms = 0.0;
    double q_tail_jitter_frames = 0.0;
    double q_disorder_frames = 0.0;
    uint64_t q_max_disorder_depth = 0;
    uint64_t q_buffered_frames = 0;
    uint64_t q_adaptive_depth = 0;
    double q_depth_raw = 0.0;
    uint64_t q_recv_delta = 0;
    uint64_t q_dlv_delta = 0;
    uint64_t q_skip_delta = 0;
    uint64_t q_drop_delta = 0;
    uint64_t q_dup_delta = 0;
    uint64_t q_late_delta = 0;
    uint64_t q_reorder_delta = 0;
    uint64_t q_stale_delta = 0;
    uint64_t q_ovf_delta = 0;
};

struct RunState
{
    explicit RunState(Clock::time_point start) : start_time(start), last_arrival(start)
    {
    }

    Clock::time_point start_time;
    Clock::time_point last_arrival;
    bool has_last_arrival = false;
    std::ofstream arrival_csv;
    std::mutex arrival_mutex;
    std::unordered_set<uint32_t> seen;

    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> send_fail{0};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> integrity_errors{0};
    std::atomic<uint64_t> duplicates{0};
};

static void usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --duration <n>         run duration in seconds (default: 30)\n"
              << "  --rate-mbps <f>        ReliableUDP send rate in Mbps, 0 = unlimited (default: 5.0)\n"
              << "  --payload-bytes <n>    ReliableUDP message size including test header (default: 1200)\n"
              << "  --sender-port <n>      local sender port (default: 15301)\n"
              << "  --receiver-port <n>    local receiver port (default: 15302)\n"
              << "  --stats-period-ms <n>  RecvQueue stats sampling period (default: 100)\n"
              << "  --out-dir <path>       output directory (default: reliable_udp_jitter_out)\n";
}

static uint64_t elapsed_us(Clock::time_point start, Clock::time_point now)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
}

static double elapsed_ms(Clock::time_point start, Clock::time_point now)
{
    return std::chrono::duration<double, std::milli>(now - start).count();
}

static uint32_t adler32_compute(const uint8_t *data, size_t len)
{
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; ++i)
    {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static bool make_dir(const std::string &path)
{
    if (path.empty())
        return false;

    if (::mkdir(path.c_str(), 0775) == 0)
        return true;

    if (errno == EEXIST)
    {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    return false;
}

static bool make_dirs(const std::string &path)
{
    if (path.empty())
        return false;

    std::string current;
    size_t pos = 0;
    if (path[0] == '/')
    {
        current = "/";
        pos = 1;
    }

    while (pos <= path.size())
    {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty())
        {
            if (!current.empty() && current[current.size() - 1] != '/')
                current += '/';
            current += part;
            if (!make_dir(current))
                return false;
        }
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }

    return true;
}

static std::string join_path(const std::string &dir, const std::string &name)
{
    if (dir.empty() || dir[dir.size() - 1] == '/')
        return dir + name;
    return dir + "/" + name;
}

static std::string trim_copy(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string{};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::string stat_value(const std::string &stats, const std::string &key)
{
    const auto pos = stats.find(key);
    if (pos == std::string::npos)
        return std::string{};

    const auto begin = pos + key.size();
    const auto end = stats.find(',', begin);
    return trim_copy(stats.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
}

static double parse_double_prefix(const std::string &value)
{
    if (value.empty())
        return 0.0;
    return std::strtod(value.c_str(), nullptr);
}

static uint64_t parse_u64_prefix(const std::string &value)
{
    if (value.empty())
        return 0;
    return static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
}

static ParsedQueueStats parse_queue_stats(const std::string &stats)
{
    ParsedQueueStats out;

    out.q_avg_fi_ms = parse_double_prefix(stat_value(stats, "q_avg_fi="));

    const std::string jitter = stat_value(stats, "q_jitter=");
    const auto jitter_slash = jitter.find('/');
    out.q_jitter_ms = parse_double_prefix(jitter);
    if (jitter_slash != std::string::npos)
        out.q_jitter_frames = parse_double_prefix(jitter.substr(jitter_slash + 1));

    const std::string tail_jitter = stat_value(stats, "q_tail_jitter=");
    const auto tail_jitter_slash = tail_jitter.find('/');
    out.q_tail_jitter_ms = parse_double_prefix(tail_jitter);
    if (tail_jitter_slash != std::string::npos)
        out.q_tail_jitter_frames = parse_double_prefix(tail_jitter.substr(tail_jitter_slash + 1));

    const std::string disorder = stat_value(stats, "q_dis=");
    const auto disorder_slash = disorder.find('/');
    out.q_disorder_frames = parse_double_prefix(disorder);
    if (disorder_slash != std::string::npos)
        out.q_max_disorder_depth = parse_u64_prefix(disorder.substr(disorder_slash + 1));

    const std::string depth = stat_value(stats, "q_depth=");
    const auto first_slash = depth.find('/');
    out.q_buffered_frames = parse_u64_prefix(depth);
    if (first_slash != std::string::npos)
        out.q_adaptive_depth = parse_u64_prefix(depth.substr(first_slash + 1));

    out.q_depth_raw = parse_double_prefix(stat_value(stats, "q_depth_raw="));
    out.q_recv_delta = parse_u64_prefix(stat_value(stats, "q_recv="));
    out.q_dlv_delta = parse_u64_prefix(stat_value(stats, "q_dlv="));
    out.q_skip_delta = parse_u64_prefix(stat_value(stats, "q_skip="));
    out.q_drop_delta = parse_u64_prefix(stat_value(stats, "q_drop="));
    out.q_dup_delta = parse_u64_prefix(stat_value(stats, "q_dup="));
    out.q_late_delta = parse_u64_prefix(stat_value(stats, "q_late="));
    out.q_reorder_delta = parse_u64_prefix(stat_value(stats, "q_reorder="));
    out.q_stale_delta = parse_u64_prefix(stat_value(stats, "q_stale="));
    out.q_ovf_delta = parse_u64_prefix(stat_value(stats, "q_ovf="));

    return out;
}

static void fill_test_frame(uint8_t *data, size_t size, uint32_t seq, uint64_t send_us)
{
    JitterMsgHdr hdr{};
    hdr.magic = TEST_MAGIC;
    hdr.seq = seq;
    hdr.send_elapsed_us = send_us;
    hdr.frame_size = static_cast<uint32_t>(size);

    const size_t payload_offset = sizeof(JitterMsgHdr);
    for (size_t i = payload_offset; i < size; ++i)
    {
        data[i] = static_cast<uint8_t>((seq * 131u + i * 17u + (i >> 3)) & 0xFFu);
    }

    hdr.payload_crc = adler32_compute(data + payload_offset, size - payload_offset);
    std::memcpy(data, &hdr, sizeof(hdr));
}

static void handle_received_frames(RunState &state, const std::vector<QueueFrame> &frames)
{
    std::lock_guard<std::mutex> lock(state.arrival_mutex);

    for (const auto &frame : frames)
    {
        const auto now = Clock::now();
        if (frame.size < sizeof(JitterMsgHdr))
        {
            state.integrity_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        JitterMsgHdr hdr{};
        std::memcpy(&hdr, frame.data, sizeof(hdr));
        const size_t payload_size = frame.size - sizeof(JitterMsgHdr);
        const uint32_t crc = adler32_compute(frame.data + sizeof(JitterMsgHdr), payload_size);

        if (hdr.magic != TEST_MAGIC || hdr.frame_size != frame.size || hdr.payload_crc != crc)
        {
            state.integrity_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (!state.seen.insert(hdr.seq).second)
        {
            state.duplicates.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const double interval = state.has_last_arrival ? elapsed_ms(state.last_arrival, now) : 0.0;
        const double latency = (static_cast<double>(elapsed_us(state.start_time, now)) -
                                static_cast<double>(hdr.send_elapsed_us)) /
                               1000.0;

        state.arrival_csv << std::fixed << std::setprecision(3) << elapsed_ms(state.start_time, now) << ','
                          << hdr.seq << ',' << interval << ',' << latency << ',' << frame.size << '\n';

        state.last_arrival = now;
        state.has_last_arrival = true;
        state.received.fetch_add(1, std::memory_order_relaxed);
    }
}

static void write_stats_header(std::ofstream &csv)
{
    csv << "elapsed_ms,q_avg_fi_ms,q_jitter_ms,q_jitter_frames,q_disorder_frames,q_max_disorder_depth,"
           "q_tail_jitter_ms,q_tail_jitter_frames,"
           "q_buffered_frames,q_adaptive_depth,q_depth_raw,q_recv_delta,q_dlv_delta,"
           "q_skip_delta,q_drop_delta,q_dup_delta,q_late_delta,q_reorder_delta,q_stale_delta,q_ovf_delta,"
           "recv_rate_bps,lost_rate\n";
}

static void stats_sampler(std::shared_ptr<ReliableUDP> receiver, RunState &state, const Config &cfg,
                          std::atomic<bool> &stop_requested, std::ofstream &csv)
{
    const auto period = std::chrono::milliseconds(cfg.stats_period_ms);
    auto next_sample = Clock::now() + period;

    while (!stop_requested.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_until(next_sample);
        next_sample += period;

        const auto now = Clock::now();
        const auto parsed = parse_queue_stats(receiver->queue_stats_text());
        csv << std::fixed << std::setprecision(3) << elapsed_ms(state.start_time, now) << ',' << std::setprecision(2)
            << parsed.q_avg_fi_ms << ',' << parsed.q_jitter_ms << ',' << parsed.q_jitter_frames << ','
            << parsed.q_disorder_frames << ',' << parsed.q_max_disorder_depth << ',' << parsed.q_tail_jitter_ms << ','
            << parsed.q_tail_jitter_frames << ',' << parsed.q_buffered_frames << ','
            << parsed.q_adaptive_depth << ',' << parsed.q_depth_raw << ','
            << parsed.q_recv_delta << ',' << parsed.q_dlv_delta << ',' << parsed.q_skip_delta << ','
            << parsed.q_drop_delta << ',' << parsed.q_dup_delta << ',' << parsed.q_late_delta << ','
            << parsed.q_reorder_delta << ',' << parsed.q_stale_delta << ',' << parsed.q_ovf_delta << ','
            << receiver->recv_rate() << ',' << receiver->lost_rate() << '\n';
    }
}

static Config parse_args(int argc, char *argv[])
{
    Config cfg;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            usage(argv[0]);
            std::exit(0);
        }

        if (i + 1 >= argc)
            throw std::runtime_error("missing value for " + arg);

        const std::string value = argv[++i];
        if (arg == "--duration")
            cfg.duration_sec = static_cast<uint32_t>(std::stoul(value));
        else if (arg == "--rate-mbps")
            cfg.rate_mbps = std::stod(value);
        else if (arg == "--payload-bytes")
            cfg.payload_bytes = static_cast<size_t>(std::stoul(value));
        else if (arg == "--sender-port")
            cfg.sender_port = static_cast<uint16_t>(std::stoul(value));
        else if (arg == "--receiver-port")
            cfg.receiver_port = static_cast<uint16_t>(std::stoul(value));
        else if (arg == "--stats-period-ms")
            cfg.stats_period_ms = static_cast<uint32_t>(std::stoul(value));
        else if (arg == "--out-dir")
            cfg.out_dir = value;
        else
            throw std::runtime_error("unknown option: " + arg);
    }

    cfg.payload_bytes = std::max(cfg.payload_bytes, sizeof(JitterMsgHdr));
    cfg.payload_bytes = std::min(cfg.payload_bytes, MAX_TRX_UDP_SIZE);
    cfg.stats_period_ms = std::max<uint32_t>(cfg.stats_period_ms, 10);
    cfg.rate_mbps = std::max(0.0, cfg.rate_mbps);

    return cfg;
}

static void write_summary(const Config &cfg, const RunState &state, const std::string &summary_path,
                          double elapsed_seconds)
{
    std::ofstream summary(summary_path.c_str(), std::ios::out | std::ios::trunc);
    if (!summary)
        throw std::runtime_error("failed to open " + summary_path);

    const auto sent = state.sent.load(std::memory_order_relaxed);
    const auto send_fail = state.send_fail.load(std::memory_order_relaxed);
    const auto received = state.received.load(std::memory_order_relaxed);
    const auto errors = state.integrity_errors.load(std::memory_order_relaxed);
    const auto duplicates = state.duplicates.load(std::memory_order_relaxed);
    const double loss_pct =
        sent > 0 ? 100.0 * (1.0 - static_cast<double>(received) / static_cast<double>(sent)) : 0.0;

    summary << std::fixed << std::setprecision(3);
    summary << "ReliableUDP TC jitter analysis\n";
    summary << "duration_sec=" << cfg.duration_sec << '\n';
    summary << "elapsed_sec=" << elapsed_seconds << '\n';
    summary << "rate_mbps=" << cfg.rate_mbps << '\n';
    summary << "payload_bytes=" << cfg.payload_bytes << '\n';
    summary << "sender_port=" << cfg.sender_port << '\n';
    summary << "receiver_port=" << cfg.receiver_port << '\n';
    summary << "stats_period_ms=" << cfg.stats_period_ms << '\n';
    summary << "out_dir=" << cfg.out_dir << '\n';
    summary << "sent_count=" << sent << '\n';
    summary << "send_fail_count=" << send_fail << '\n';
    summary << "received_count=" << received << '\n';
    summary << "integrity_errors=" << errors << '\n';
    summary << "duplicate_count=" << duplicates << '\n';
    summary << "observed_message_loss_pct=" << loss_pct << '\n';
    summary << "plot_command=python3 tests/plot_reliable_udp_jitter.py " << cfg.out_dir << '\n';
}

static void run_test(const Config &cfg)
{
    if (!make_dirs(cfg.out_dir))
        throw std::runtime_error("failed to create output directory: " + cfg.out_dir);

    const std::string arrival_path = join_path(cfg.out_dir, "arrival_intervals.csv");
    const std::string stats_path = join_path(cfg.out_dir, "queue_stats.csv");
    const std::string summary_path = join_path(cfg.out_dir, "summary.txt");

    auto &ioc = BG_SERVICE;
    const auto start = Clock::now();
    RunState state(start);

    state.arrival_csv.open(arrival_path.c_str(), std::ios::out | std::ios::trunc);
    if (!state.arrival_csv)
        throw std::runtime_error("failed to open " + arrival_path);
    state.arrival_csv << "elapsed_ms,seq,interval_ms,latency_ms,size_bytes\n";

    std::ofstream stats_csv(stats_path.c_str(), std::ios::out | std::ios::trunc);
    if (!stats_csv)
        throw std::runtime_error("failed to open " + stats_path);
    write_stats_header(stats_csv);

    auto receiver = std::make_shared<ReliableUDP>(ioc, cfg.receiver_port);
    receiver->set_receive_callback([&state](const std::vector<QueueFrame> &frames) {
        handle_received_frames(state, frames);
        return true;
    });
    receiver->start();

    auto sender = std::make_shared<ReliableUDP>(ioc, cfg.sender_port);
    sender->start();
    if (!sender->add_destination("127.0.0.1", cfg.receiver_port))
        throw std::runtime_error("sender add_destination failed");

    std::atomic<bool> stop_stats{false};
    std::thread stats_thread(stats_sampler, receiver, std::ref(state), std::cref(cfg), std::ref(stop_stats),
                             std::ref(stats_csv));

    const auto deadline = start + std::chrono::seconds(cfg.duration_sec);
    const double bytes_per_sec = cfg.rate_mbps > 0.0 ? cfg.rate_mbps * 1e6 / 8.0 : 0.0;
    double byte_budget = static_cast<double>(cfg.payload_bytes);
    auto last_budget_update = Clock::now();
    uint32_t seq = 0;

    while (Clock::now() < deadline)
    {
        if (bytes_per_sec > 0.0)
        {
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last_budget_update).count();
            byte_budget += dt * bytes_per_sec;
            last_budget_update = now;

            if (byte_budget < static_cast<double>(cfg.payload_bytes))
            {
                const double wait_sec = (static_cast<double>(cfg.payload_bytes) - byte_budget) / bytes_per_sec;
                std::this_thread::sleep_for(std::chrono::duration<double>(wait_sec));
                continue;
            }
            byte_budget -= static_cast<double>(cfg.payload_bytes);
        }

        const uint32_t frame_seq = seq++;
        const uint64_t send_us = elapsed_us(start, Clock::now());
        if (sender->send_fill(cfg.payload_bytes, [frame_seq, send_us](uint8_t *data, size_t size) {
                fill_test_frame(data, size, frame_seq, send_us);
            }))
        {
            state.sent.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.send_fail.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop_stats.store(true, std::memory_order_release);
    if (stats_thread.joinable())
        stats_thread.join();

    sender->stop();
    receiver->stop();

    {
        std::lock_guard<std::mutex> lock(state.arrival_mutex);
        state.arrival_csv.flush();
    }
    stats_csv.flush();

    const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    write_summary(cfg, state, summary_path, elapsed_seconds);

    std::cout << "ReliableUDP TC jitter analysis complete\n";
    std::cout << "  Output directory : " << cfg.out_dir << '\n';
    std::cout << "  Sent / received  : " << state.sent.load() << " / " << state.received.load() << '\n';
    std::cout << "  Integrity errors : " << state.integrity_errors.load() << '\n';
    std::cout << "  Duplicates       : " << state.duplicates.load() << '\n';
    std::cout << "  Plot command     : python3 tests/plot_reliable_udp_jitter.py " << cfg.out_dir << '\n';
}
} // namespace

int main(int argc, char *argv[])
{
    try
    {
        const Config cfg = parse_args(argc, argv);
        run_test(cfg);
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        usage(argv[0]);
        return 1;
    }

    return 0;
}
