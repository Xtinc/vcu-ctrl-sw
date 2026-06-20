#include "lib_network/ReliableUDP.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

extern "C"
{
#include "lib_rtos/message.h"
}

namespace
{
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

constexpr uint32_t TEST_MAGIC = 0x524a4e54u; // RJNT
constexpr const char *ARRIVAL_HEADER =
    "elapsed_ms,seq,interval_ms,latency_ms,size_bytes,clock_synced,rtt_ms,offset_ms";
constexpr const char *STATS_HEADER =
    "elapsed_ms,q_avg_fi_ms,q_fb_fi_ms,q_out_fi_ms,q_jitter_ms,q_jitter_frames,q_disorder_frames,"
    "q_max_disorder_depth,q_tail_jitter_ms,q_tail_jitter_frames,q_buffered_frames,q_adaptive_depth,"
    "q_depth_raw,q_depth_error_frames,q_pressure_frames,q_pacing_factor,q_recv_delta,q_dlv_delta,"
    "q_skip_delta,q_drop_delta,q_dup_delta,q_late_delta,q_reorder_delta,q_stale_delta,q_ovf_delta,"
    "recv_rate_bps,lost_rate";

struct NetworkHeader
{
    uint32_t magic;
    uint32_t seq;
    uint64_t send_wall_us;
    uint32_t frame_size;
    uint32_t payload_crc;
};
static_assert(sizeof(NetworkHeader) == 24, "NetworkHeader layout changed");
static_assert(offsetof(NetworkHeader, send_wall_us) == 8, "NetworkHeader timestamp offset changed");

struct Config
{
    std::string role;
    std::string peer_address;
    std::string out_dir = "reliable_udp_network_out";
    uint16_t local_port = 15301;
    uint16_t peer_port = 15302;
    uint32_t duration_sec = 30;
    uint32_t stats_period_ms = 100;
    size_t payload_bytes = 1200;
    double rate_mbps = 5.0;
};

struct State
{
    explicit State(SteadyClock::time_point start) : start(start) {}
    SteadyClock::time_point start;
    SteadyClock::time_point last_arrival{};
    bool has_last_arrival = false;
    std::atomic<bool> first_frame{false};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> send_fail{0};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> integrity_errors{0};
    std::atomic<uint64_t> duplicates{0};
    std::mutex mutex;
    std::set<uint32_t> seen;
    std::ofstream arrivals;
};

double elapsed_ms(SteadyClock::time_point start, SteadyClock::time_point now)
{
    return std::chrono::duration<double, std::milli>(now - start).count();
}

uint64_t wall_time_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SystemClock::now().time_since_epoch()).count());
}

uint32_t adler32_compute(const uint8_t *data, size_t size)
{
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < size; ++i)
    {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

double percentile(const std::vector<double> &sorted_values, double quantile)
{
    if (sorted_values.empty())
        return 0.0;
    const double position = quantile * static_cast<double>(sorted_values.size() - 1);
    const size_t lower = static_cast<size_t>(position);
    const size_t upper = std::min(lower + 1, sorted_values.size() - 1);
    return sorted_values[lower] + (position - static_cast<double>(lower)) *
                                      (sorted_values[upper] - sorted_values[lower]);
}

bool make_one_dir(const std::string &path)
{
#if defined(_WIN32)
    if (_mkdir(path.c_str()) == 0)
#else
    if (::mkdir(path.c_str(), 0775) == 0)
#endif
        return true;
    if (errno != EEXIST)
        return false;
#if defined(_WIN32)
    struct _stat info;
    return ::_stat(path.c_str(), &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

bool make_dirs(std::string path)
{
    if (path.empty())
        return false;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string current;
    size_t pos = 0;
    if (path.size() > 1 && path[1] == ':')
    {
        current = path.substr(0, 2);
        pos = 2;
    }
    else if (path[0] == '/')
    {
        current = "/";
        pos = 1;
    }
    while (pos <= path.size())
    {
        const size_t slash = path.find('/', pos);
        const auto part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty())
        {
            if (!current.empty() && current.back() != '/')
                current += '/';
            current += part;
            if (!make_one_dir(current))
                return false;
        }
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return true;
}

std::string path_join(const std::string &dir, const std::string &name)
{
    return dir.empty() || dir.back() == '/' || dir.back() == '\\' ? dir + name : dir + "/" + name;
}

std::string stat_value(const std::string &stats, const std::string &key)
{
    const auto pos = stats.find(key);
    if (pos == std::string::npos)
        return "0";
    const auto begin = pos + key.size();
    const auto end = stats.find(',', begin);
    return stats.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

double part(const std::string &value, size_t index = 0)
{
    size_t begin = 0;
    for (size_t i = 0; i < index; ++i)
    {
        begin = value.find('/', begin);
        if (begin == std::string::npos)
            return 0.0;
        ++begin;
    }
    return std::strtod(value.c_str() + begin, nullptr);
}

void write_stats_row(std::ofstream &csv, ReliableUDP &udp, double elapsed)
{
    const auto stats = udp.queue_stats_text();
    const auto jitter = stat_value(stats, "q_jitter=");
    const auto tail = stat_value(stats, "q_tail_jitter=");
    const auto disorder = stat_value(stats, "q_dis=");
    const auto depth = stat_value(stats, "q_depth=");
    csv << std::fixed << std::setprecision(3) << elapsed << ','
        << part(stat_value(stats, "q_avg_fi=")) << ',' << part(stat_value(stats, "q_fb_fi=")) << ','
        << part(stat_value(stats, "q_out_fi=")) << ',' << part(jitter) << ',' << part(jitter, 1) << ','
        << part(disorder) << ',' << part(disorder, 1) << ',' << part(tail) << ',' << part(tail, 1) << ','
        << part(depth) << ',' << part(depth, 1) << ',' << part(stat_value(stats, "q_depth_raw=")) << ','
        << part(stat_value(stats, "q_depth_err=")) << ',' << part(stat_value(stats, "q_pressure=")) << ','
        << part(stat_value(stats, "q_pace=")) << ',' << part(stat_value(stats, "q_recv=")) << ','
        << part(stat_value(stats, "q_dlv=")) << ',' << part(stat_value(stats, "q_skip=")) << ','
        << part(stat_value(stats, "q_drop=")) << ',' << part(stat_value(stats, "q_dup=")) << ','
        << part(stat_value(stats, "q_late=")) << ',' << part(stat_value(stats, "q_reorder=")) << ','
        << part(stat_value(stats, "q_stale=")) << ',' << part(stat_value(stats, "q_ovf=")) << ','
        << udp.recv_rate() << ',' << udp.lost_rate() << '\n';
}

void fill_frame(uint8_t *data, size_t size, uint32_t seq)
{
    NetworkHeader header{};
    header.magic = TEST_MAGIC;
    header.seq = seq;
    header.send_wall_us = wall_time_us();
    header.frame_size = static_cast<uint32_t>(size);
    for (size_t i = sizeof(header); i < size; ++i)
        data[i] = static_cast<uint8_t>((seq * 131u + i * 17u + (i >> 3)) & 0xffu);
    header.payload_crc = adler32_compute(data + sizeof(header), size - sizeof(header));
    std::memcpy(data, &header, sizeof(header));
}

Config parse_args(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " --role sender|receiver --peer-address <ip> [--local-port n] [--peer-port n]"
                         " [--duration n] [--rate-mbps f] [--payload-bytes n] [--stats-period-ms n]"
                         " [--out-dir path]\n";
            std::exit(0);
        }
        if (i + 1 >= argc)
            throw std::runtime_error("missing value for " + arg);
        const std::string value = argv[++i];
        if (arg == "--role") cfg.role = value;
        else if (arg == "--peer-address") cfg.peer_address = value;
        else if (arg == "--local-port") cfg.local_port = static_cast<uint16_t>(std::stoul(value));
        else if (arg == "--peer-port") cfg.peer_port = static_cast<uint16_t>(std::stoul(value));
        else if (arg == "--duration") cfg.duration_sec = static_cast<uint32_t>(std::stoul(value));
        else if (arg == "--rate-mbps") cfg.rate_mbps = std::stod(value);
        else if (arg == "--payload-bytes") cfg.payload_bytes = static_cast<size_t>(std::stoul(value));
        else if (arg == "--stats-period-ms") cfg.stats_period_ms = static_cast<uint32_t>(std::stoul(value));
        else if (arg == "--out-dir") cfg.out_dir = value;
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (cfg.role != "sender" && cfg.role != "receiver")
        throw std::runtime_error("--role must be sender or receiver");
    if (cfg.peer_address.empty())
        throw std::runtime_error("--peer-address is required");
    cfg.payload_bytes = std::max(cfg.payload_bytes, sizeof(NetworkHeader));
    cfg.payload_bytes = std::min(cfg.payload_bytes, MAX_TRX_UDP_SIZE);
    cfg.stats_period_ms = std::max<uint32_t>(cfg.stats_period_ms, 10);
    cfg.duration_sec = std::max<uint32_t>(cfg.duration_sec, 1);
    cfg.rate_mbps = std::max(0.0, cfg.rate_mbps);
    return cfg;
}

void write_common_summary(std::ofstream &out, const Config &cfg)
{
    out << "role=" << cfg.role << '\n' << "duration_sec=" << cfg.duration_sec << '\n'
        << "local_port=" << cfg.local_port << '\n' << "peer_address=" << cfg.peer_address << '\n'
        << "peer_port=" << cfg.peer_port << '\n' << "rate_mbps=" << cfg.rate_mbps << '\n'
        << "payload_bytes=" << cfg.payload_bytes << '\n';
}

void run_sender(const Config &cfg)
{
    if (!make_dirs(cfg.out_dir))
        throw std::runtime_error("cannot create output directory " + cfg.out_dir);
    auto udp = std::make_shared<ReliableUDP>(BG_SERVICE, cfg.local_port);
    udp->start();
    if (!udp->add_destination(cfg.peer_address, cfg.peer_port))
        throw std::runtime_error("add_destination failed");

    const auto sync_deadline = SteadyClock::now() + std::chrono::seconds(3);
    while (!udp->is_time_synced() && SteadyClock::now() < sync_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    State state(SteadyClock::now());
    const auto deadline = state.start + std::chrono::seconds(cfg.duration_sec);
    const double bytes_per_sec = cfg.rate_mbps * 1e6 / 8.0;
    double budget = static_cast<double>(cfg.payload_bytes);
    auto budget_time = SteadyClock::now();
    auto last_send_time = SteadyClock::time_point{};
    std::vector<double> send_intervals_ms;
    uint32_t seq = 0;
    while (SteadyClock::now() < deadline)
    {
        if (bytes_per_sec > 0.0)
        {
            const auto now = SteadyClock::now();
            budget += std::chrono::duration<double>(now - budget_time).count() * bytes_per_sec;
            budget_time = now;
            if (budget < cfg.payload_bytes)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            budget -= cfg.payload_bytes;
        }
        const auto frame_seq = seq++;
        if (udp->send_fill(cfg.payload_bytes, [frame_seq](uint8_t *data, size_t size) { fill_frame(data, size, frame_seq); }))
        {
            const auto send_time = SteadyClock::now();
            if (last_send_time != SteadyClock::time_point{})
                send_intervals_ms.push_back(elapsed_ms(last_send_time, send_time));
            last_send_time = send_time;
            ++state.sent;
        }
        else
            ++state.send_fail;
    }
    const double send_elapsed_sec = std::chrono::duration<double>(SteadyClock::now() - state.start).count();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const bool synced = udp->is_time_synced();
    const auto rtt = udp->rtt_ms();
    const auto offset = udp->offset_ms();
    udp->stop();

    std::sort(send_intervals_ms.begin(), send_intervals_ms.end());
    const double interval_sum = std::accumulate(send_intervals_ms.begin(), send_intervals_ms.end(), 0.0);
    const double interval_mean = send_intervals_ms.empty() ? 0.0 : interval_sum / send_intervals_ms.size();
    const double payload_rate_mbps = send_elapsed_sec > 0.0
                                         ? static_cast<double>(state.sent.load() * cfg.payload_bytes) * 8.0 /
                                               send_elapsed_sec / 1e6
                                         : 0.0;

    std::ofstream summary(path_join(cfg.out_dir, "sender_summary.txt"));
    write_common_summary(summary, cfg);
    summary << std::fixed << std::setprecision(3)
            << "clock_synced=" << (synced ? 1 : 0) << '\n' << "rtt_ms=" << rtt << '\n'
            << "offset_ms=" << offset << '\n' << "sent_count=" << state.sent.load() << '\n'
            << "send_fail_count=" << state.send_fail.load() << '\n'
            << "send_elapsed_sec=" << send_elapsed_sec << '\n'
            << "payload_rate_mbps=" << payload_rate_mbps << '\n'
            << "send_interval_count=" << send_intervals_ms.size() << '\n'
            << "send_interval_mean_ms=" << interval_mean << '\n'
            << "send_interval_p50_ms=" << percentile(send_intervals_ms, 0.50) << '\n'
            << "send_interval_p95_ms=" << percentile(send_intervals_ms, 0.95) << '\n'
            << "send_interval_p99_ms=" << percentile(send_intervals_ms, 0.99) << '\n'
            << "send_interval_max_ms=" << (send_intervals_ms.empty() ? 0.0 : send_intervals_ms.back()) << '\n';
    std::cout << "Sender complete: sent=" << state.sent.load() << " failed=" << state.send_fail.load() << '\n';
}

void run_receiver(const Config &cfg)
{
    if (!make_dirs(cfg.out_dir))
        throw std::runtime_error("cannot create output directory " + cfg.out_dir);
    State state(SteadyClock::now());
    state.arrivals.open(path_join(cfg.out_dir, "arrival_intervals.csv"));
    state.arrivals << ARRIVAL_HEADER << '\n';
    std::ofstream stats(path_join(cfg.out_dir, "queue_stats.csv"));
    stats << STATS_HEADER << '\n';

    std::shared_ptr<ReliableUDP> udp;
    udp = std::make_shared<ReliableUDP>(BG_SERVICE, cfg.local_port);
    udp->set_receive_callback([&state, &udp](const std::vector<QueueFrame> &frames, bool) {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (const auto &frame : frames)
        {
            const auto steady_now = SteadyClock::now();
            NetworkHeader header{};
            if (frame.size < sizeof(header))
            {
                ++state.integrity_errors;
                continue;
            }
            std::memcpy(&header, frame.data, sizeof(header));
            const auto crc = adler32_compute(frame.data + sizeof(header), frame.size - sizeof(header));
            if (header.magic != TEST_MAGIC || header.frame_size != frame.size || header.payload_crc != crc)
            {
                ++state.integrity_errors;
                continue;
            }
            if (!state.seen.insert(header.seq).second)
            {
                ++state.duplicates;
                continue;
            }
            const bool synced = udp->is_time_synced();
            const double interval = state.has_last_arrival ? elapsed_ms(state.last_arrival, steady_now) : 0.0;
            const double latency = synced ? (static_cast<double>(wall_time_us()) -
                                             static_cast<double>(header.send_wall_us) +
                                             static_cast<double>(udp->offset_ms()) * 1000.0) / 1000.0 : -1.0;
            state.arrivals << std::fixed << std::setprecision(3) << elapsed_ms(state.start, steady_now) << ','
                           << header.seq << ',' << interval << ',' << latency << ',' << frame.size << ','
                           << (synced ? 1 : 0) << ',' << udp->rtt_ms() << ',' << udp->offset_ms() << '\n';
            state.last_arrival = steady_now;
            state.has_last_arrival = true;
            state.first_frame = true;
            ++state.received;
        }
        return true;
    });
    udp->start();
    if (!udp->add_destination(cfg.peer_address, cfg.peer_port))
        throw std::runtime_error("add_destination failed");

    const auto first_deadline = SteadyClock::now() + std::chrono::seconds(60);
    auto next_stats = SteadyClock::now();
    while (!state.first_frame && SteadyClock::now() < first_deadline)
    {
        if (SteadyClock::now() >= next_stats)
        {
            write_stats_row(stats, *udp, elapsed_ms(state.start, SteadyClock::now()));
            next_stats += std::chrono::milliseconds(cfg.stats_period_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!state.first_frame)
        throw std::runtime_error("timed out waiting for the first valid frame");

    const auto deadline = SteadyClock::now() + std::chrono::seconds(cfg.duration_sec + 3);
    while (SteadyClock::now() < deadline)
    {
        const auto now = SteadyClock::now();
        if (now >= next_stats)
        {
            write_stats_row(stats, *udp, elapsed_ms(state.start, now));
            next_stats += std::chrono::milliseconds(cfg.stats_period_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool synced = udp->is_time_synced();
    const auto rtt = udp->rtt_ms();
    const auto offset = udp->offset_ms();
    udp->stop();
    state.arrivals.flush();
    stats.flush();

    std::ofstream summary(path_join(cfg.out_dir, "receiver_summary.txt"));
    write_common_summary(summary, cfg);
    summary << "clock_synced=" << (synced ? 1 : 0) << '\n' << "rtt_ms=" << rtt << '\n'
            << "offset_ms=" << offset << '\n' << "received_count=" << state.received.load() << '\n'
            << "integrity_errors=" << state.integrity_errors.load() << '\n'
            << "duplicate_count=" << state.duplicates.load() << '\n';
    std::cout << "Receiver complete: received=" << state.received.load()
              << " errors=" << state.integrity_errors.load() << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    message_init();
    try
    {
        const auto cfg = parse_args(argc, argv);
        if (cfg.role == "sender")
            run_sender(cfg);
        else
            run_receiver(cfg);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
