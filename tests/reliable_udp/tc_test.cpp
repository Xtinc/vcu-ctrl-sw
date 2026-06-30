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
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr uint32_t TEST_MAGIC = 0x524a5443u; // RJTC
constexpr size_t SEND_INTERVAL_RESERVOIR_SIZE = 65536;
constexpr const char *CSV_CONTRACT_VERSION = "reliable_udp_test_v1";
constexpr const char *ARRIVAL_CSV_HEADER =
    "elapsed_s,seq,interval_ms,latency_ms,size_bytes,allow_immediate";
constexpr const char *INPUT_CSV_HEADER = "elapsed_s,interval_ms";
constexpr const char *SENDER_CSV_HEADER =
    "sent_count,send_fail_count,send_elapsed_s,payload_rate_mbps,interval_mean_ms,interval_p50_ms,"
    "interval_p95_ms,interval_p99_ms,interval_max_ms";

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
    size_t payload_bytes = 1300;
    uint16_t sender_port = 15301;
    uint16_t receiver_port = 15302;
    std::string out_dir = "reliable_udp_jitter_out";
};

struct SendIntervalStats
{
    SendIntervalStats() : rng(TEST_MAGIC)
    {
        sample.reserve(SEND_INTERVAL_RESERVOIR_SIZE);
    }

    void add(double value)
    {
        ++count;
        sum += value;
        maximum = std::max(maximum, value);
        if (sample.size() < SEND_INTERVAL_RESERVOIR_SIZE)
        {
            sample.push_back(value);
            return;
        }
        std::uniform_int_distribution<uint64_t> distribution(0, count - 1);
        const auto index = distribution(rng);
        if (index < sample.size())
            sample[static_cast<size_t>(index)] = value;
    }

    uint64_t count = 0;
    double sum = 0.0;
    double maximum = 0.0;
    std::vector<double> sample;
    std::mt19937_64 rng;
};

struct RunState
{
    explicit RunState(Clock::time_point start) : start_time(start), last_arrival(start)
    {
    }

    Clock::time_point start_time;
    Clock::time_point last_arrival;
    Clock::time_point last_input{};
    Clock::time_point last_send{};
    bool has_last_arrival = false;
    bool has_last_input = false;
    bool has_last_send = false;
    SendIntervalStats send_intervals;
    std::ofstream arrival_csv;
    std::ofstream input_csv;
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
              << "  --payload-bytes <n>    ReliableUDP message size including test header (default: 1300)\n"
              << "  --sender-port <n>      local sender port (default: 15301)\n"
              << "  --receiver-port <n>    local receiver port (default: 15302)\n"
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

static double elapsed_s(Clock::time_point start, Clock::time_point now)
{
    return std::chrono::duration<double>(now - start).count();
}

static double percentile(const std::vector<double> &sorted, double quantile)
{
    if (sorted.empty())
        return 0.0;
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    return sorted[lower] + (position - static_cast<double>(lower)) * (sorted[upper] - sorted[lower]);
}

static void handle_controller_input(RunState &state, Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(state.arrival_mutex);
    const double interval = state.has_last_input ? elapsed_ms(state.last_input, now) : 0.0;
    state.input_csv << std::fixed << std::setprecision(3) << elapsed_s(state.start_time, now) << ',' << interval
                    << '\n';
    state.last_input = now;
    state.has_last_input = true;
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

static std::string absolute_path(const std::string &path)
{
    if (!path.empty() && path[0] == '/')
        return path;
    char cwd[4096]{};
    if (!::getcwd(cwd, sizeof(cwd)))
        throw std::runtime_error("cannot determine current directory");
    return join_path(cwd, path);
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

static void handle_received_frames(RunState &state, const std::vector<QueueFrame> &frames, bool allow_immediate)
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
        const double latency =
            (static_cast<double>(elapsed_us(state.start_time, now)) - static_cast<double>(hdr.send_elapsed_us)) /
            1000.0;

        state.arrival_csv << std::fixed << std::setprecision(3) << elapsed_s(state.start_time, now) << ',' << hdr.seq
                          << ',' << interval << ',' << latency << ',' << frame.size << ','
                          << (allow_immediate ? 1 : 0) << '\n';

        state.last_arrival = now;
        state.has_last_arrival = true;
        state.received.fetch_add(1, std::memory_order_relaxed);
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
        else if (arg == "--out-dir")
            cfg.out_dir = value;
        else
            throw std::runtime_error("unknown option: " + arg);
    }

    cfg.payload_bytes = std::max(cfg.payload_bytes, sizeof(JitterMsgHdr));
    cfg.payload_bytes = std::min(cfg.payload_bytes, MAX_TRX_UDP_SIZE);
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
    const double loss_pct = sent > 0 ? 100.0 * (1.0 - static_cast<double>(received) / static_cast<double>(sent)) : 0.0;

    summary << std::fixed << std::setprecision(3);
    summary << "ReliableUDP TC jitter analysis\n";
    summary << "csv_contract_version=" << CSV_CONTRACT_VERSION << '\n';
    summary << "duration_sec=" << cfg.duration_sec << '\n';
    summary << "elapsed_sec=" << elapsed_seconds << '\n';
    summary << "rate_mbps=" << cfg.rate_mbps << '\n';
    summary << "payload_bytes=" << cfg.payload_bytes << '\n';
    summary << "sender_port=" << cfg.sender_port << '\n';
    summary << "receiver_port=" << cfg.receiver_port << '\n';
    summary << "stats_period_ms=100 (receiver-triggered, approximate)\n";
    summary << "out_dir=" << cfg.out_dir << '\n';
    summary << "sent_count=" << sent << '\n';
    summary << "send_fail_count=" << send_fail << '\n';
    summary << "received_count=" << received << '\n';
    summary << "integrity_errors=" << errors << '\n';
    summary << "duplicate_count=" << duplicates << '\n';
    summary << "observed_message_loss_pct=" << loss_pct << '\n';
    summary << "plot_command=python3 tests/reliable_udp/plot.py " << cfg.out_dir << '\n';
    summary << "primary_figures=output_smoothness.png,controller_terms.png,network_effects.png\n";
}

static void run_test(const Config &cfg)
{
    if (!make_dirs(cfg.out_dir))
        throw std::runtime_error("failed to create output directory: " + cfg.out_dir);

    const std::string output_dir = absolute_path(cfg.out_dir);
    if (::chdir(output_dir.c_str()) != 0)
        throw std::runtime_error("failed to enter output directory: " + output_dir);

    const std::string arrival_path = "arrival_intervals.csv";
    const std::string input_path = "input_intervals.csv";
    const std::string summary_path = "summary.txt";
    std::ofstream capture_meta("capture_meta.csv", std::ios::out | std::ios::trunc);
    capture_meta << "contract_version,test_kind\n" << CSV_CONTRACT_VERSION << ",tc\n";
    capture_meta.close();

    auto &ioc = BG_SERVICE;
    const auto start = Clock::now();
    RunState state(start);

    state.arrival_csv.open(arrival_path.c_str(), std::ios::out | std::ios::trunc);
    if (!state.arrival_csv)
        throw std::runtime_error("failed to open " + arrival_path);
    state.arrival_csv << ARRIVAL_CSV_HEADER << '\n';
    state.input_csv.open(input_path.c_str(), std::ios::out | std::ios::trunc);
    if (!state.input_csv)
        throw std::runtime_error("failed to open " + input_path);
    state.input_csv << INPUT_CSV_HEADER << '\n';

    auto receiver = std::make_shared<ReliableUDP>(ioc, cfg.receiver_port);
    receiver->set_observe_callback([&state](Clock::time_point now) { handle_controller_input(state, now); });
    receiver->set_receive_callback([&state](const std::vector<QueueFrame> &frames, bool allow_immediate) {
        handle_received_frames(state, frames, allow_immediate);
        return true;
    });
    receiver->start();

    auto sender = std::make_shared<ReliableUDP>(ioc, cfg.sender_port);
    sender->start();
    if (!sender->add_destination("127.0.0.1", cfg.receiver_port))
        throw std::runtime_error("sender add_destination failed");

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
            const auto sent_now = Clock::now();
            if (state.has_last_send)
                state.send_intervals.add(elapsed_ms(state.last_send, sent_now));
            state.last_send = sent_now;
            state.has_last_send = true;
            state.sent.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.send_fail.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    sender->stop();
    receiver->stop();

    {
        std::lock_guard<std::mutex> lock(state.arrival_mutex);
        state.arrival_csv.flush();
        state.input_csv.flush();
    }
    const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    write_summary(cfg, state, summary_path, elapsed_seconds);
    std::sort(state.send_intervals.sample.begin(), state.send_intervals.sample.end());
    const double interval_mean =
        state.send_intervals.count == 0 ? 0.0 : state.send_intervals.sum / state.send_intervals.count;
    const double payload_rate = elapsed_seconds > 0.0
                                    ? static_cast<double>(state.sent.load()) * cfg.payload_bytes * 8.0 /
                                          elapsed_seconds / 1e6
                                    : 0.0;
    std::ofstream sender_stats("sender_stats.csv", std::ios::out | std::ios::trunc);
    sender_stats << SENDER_CSV_HEADER << '\n' << state.sent.load() << ',' << state.send_fail.load() << ','
                 << std::fixed << std::setprecision(6) << elapsed_seconds << ',' << payload_rate << ','
                 << interval_mean << ',' << percentile(state.send_intervals.sample, 0.50) << ','
                 << percentile(state.send_intervals.sample, 0.95) << ','
                 << percentile(state.send_intervals.sample, 0.99) << ',' << state.send_intervals.maximum << '\n';

    std::cout << "ReliableUDP TC jitter analysis complete\n";
    std::cout << "  Output directory : " << cfg.out_dir << '\n';
    std::cout << "  Sent / received  : " << state.sent.load() << " / " << state.received.load() << '\n';
    std::cout << "  Integrity errors : " << state.integrity_errors.load() << '\n';
    std::cout << "  Duplicates       : " << state.duplicates.load() << '\n';
    std::cout << "  Plot command     : python3 tests/reliable_udp/plot.py " << cfg.out_dir << '\n';
}
} // namespace

int main(int argc, char *argv[])
{
    message_init();
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
