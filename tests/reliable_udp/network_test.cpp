#include "lib_network/ReliableUDP.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
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
#include <unistd.h>
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
constexpr size_t SEND_INTERVAL_RESERVOIR_SIZE = 65536;
constexpr size_t RECENT_SEQUENCE_WINDOW_SIZE = 4096;
constexpr const char *CSV_CONTRACT_VERSION = "reliable_udp_test_v1";
constexpr const char *ARRIVAL_HEADER =
    "elapsed_s,seq,interval_ms,latency_ms,size_bytes,allow_immediate";
constexpr const char *INPUT_HEADER = "elapsed_s,interval_ms";
constexpr const char *SENDER_HEADER =
    "sent_count,send_fail_count,send_elapsed_s,payload_rate_mbps,interval_mean_ms,interval_p50_ms,"
    "interval_p95_ms,interval_p99_ms,interval_max_ms";

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
    size_t payload_bytes = 1300;
    double rate_mbps = 5.0;
};

struct IntervalStats
{
    IntervalStats() : rng(0x524a4e54u)
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
        std::uniform_int_distribution<uint64_t> pick(0, count - 1);
        const auto index = pick(rng);
        if (index < sample.size())
            sample[static_cast<size_t>(index)] = value;
    }

    std::vector<double> sorted_sample() const
    {
        auto sorted = sample;
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    }

    uint64_t count = 0;
    double sum = 0.0;
    double maximum = 0.0;
    std::vector<double> sample;
    std::mt19937_64 rng;
};

struct SenderState
{
    explicit SenderState(SteadyClock::time_point start) : start(start) {}
    SteadyClock::time_point start;
    uint64_t sent = 0;
    uint64_t send_fail = 0;
    IntervalStats intervals;
};

struct ReceiverState
{
    explicit ReceiverState(SteadyClock::time_point start, const std::string &arrival_path,
                           const std::string &input_path)
        : start(start), next_flush(start + std::chrono::seconds(1)), arrivals(arrival_path), inputs(input_path)
    {
        arrivals << ARRIVAL_HEADER << '\n';
        inputs << INPUT_HEADER << '\n';
    }

    void note_input(SteadyClock::time_point now)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const double interval = has_last_input
                                    ? std::chrono::duration<double, std::milli>(now - last_input).count()
                                    : 0.0;
        const double elapsed = std::chrono::duration<double>(now - start).count();
        inputs << std::fixed << std::setprecision(3) << elapsed << ',' << interval << '\n';
        last_input = now;
        has_last_input = true;
    }

    bool remember_sequence(uint32_t seq)
    {
        if (!recent_sequences.insert(seq).second)
            return false;
        sequence_order.push_back(seq);
        if (sequence_order.size() > RECENT_SEQUENCE_WINDOW_SIZE)
        {
            recent_sequences.erase(sequence_order.front());
            sequence_order.pop_front();
        }
        return true;
    }

    void flush_if_due(SteadyClock::time_point now)
    {
        if (now >= next_flush)
        {
            std::lock_guard<std::mutex> lock(mutex);
            arrivals.flush();
            inputs.flush();
            next_flush = now + std::chrono::seconds(1);
        }
    }

    SteadyClock::time_point start;
    SteadyClock::time_point next_flush;
    SteadyClock::time_point last_arrival{};
    SteadyClock::time_point last_input{};
    bool has_last_arrival = false;
    bool has_last_input = false;
    std::atomic<bool> first_frame{false};
    uint64_t received = 0;
    uint64_t integrity_errors = 0;
    uint64_t duplicates = 0;
    std::mutex mutex;
    std::set<uint32_t> recent_sequences;
    std::deque<uint32_t> sequence_order;
    std::ofstream arrivals;
    std::ofstream inputs;
};

double elapsed_ms(SteadyClock::time_point start, SteadyClock::time_point now)
{
    return std::chrono::duration<double, std::milli>(now - start).count();
}

double elapsed_s(SteadyClock::time_point start, SteadyClock::time_point now)
{
    return std::chrono::duration<double>(now - start).count();
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

std::string absolute_path(const std::string &path)
{
#if defined(_WIN32)
    if (path.size() > 1 && path[1] == ':')
        return path;
    char cwd[_MAX_PATH]{};
    if (!_getcwd(cwd, sizeof(cwd)))
        throw std::runtime_error("cannot determine current directory");
#else
    if (!path.empty() && path[0] == '/')
        return path;
    char cwd[4096]{};
    if (!::getcwd(cwd, sizeof(cwd)))
        throw std::runtime_error("cannot determine current directory");
#endif
    return path_join(cwd, path);
}

bool change_directory(const std::string &path)
{
#if defined(_WIN32)
    return _chdir(path.c_str()) == 0;
#else
    return ::chdir(path.c_str()) == 0;
#endif
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
                         " [--duration n] [--rate-mbps f] [--payload-bytes n] [--out-dir path]\n";
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
        else if (arg == "--out-dir") cfg.out_dir = value;
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (cfg.role != "sender" && cfg.role != "receiver")
        throw std::runtime_error("--role must be sender or receiver");
    if (cfg.peer_address.empty())
        throw std::runtime_error("--peer-address is required");
    cfg.payload_bytes = std::max(cfg.payload_bytes, sizeof(NetworkHeader));
    cfg.payload_bytes = std::min(cfg.payload_bytes, MAX_TRX_UDP_SIZE);
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

    SenderState state(SteadyClock::now());
    const auto deadline = state.start + std::chrono::seconds(cfg.duration_sec);
    const double bytes_per_sec = cfg.rate_mbps * 1e6 / 8.0;
    double budget = static_cast<double>(cfg.payload_bytes);
    auto budget_time = SteadyClock::now();
    auto last_send_time = SteadyClock::time_point{};
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
                state.intervals.add(elapsed_ms(last_send_time, send_time));
            last_send_time = send_time;
            ++state.sent;
        }
        else
            ++state.send_fail;
    }
    const double send_elapsed_sec = std::chrono::duration<double>(SteadyClock::now() - state.start).count();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    udp->stop();

    const auto sorted_intervals = state.intervals.sorted_sample();
    const double interval_mean = state.intervals.count == 0 ? 0.0 : state.intervals.sum / state.intervals.count;
    const double payload_rate_mbps = send_elapsed_sec > 0.0
                                         ? static_cast<double>(state.sent * cfg.payload_bytes) * 8.0 /
                                               send_elapsed_sec / 1e6
                                         : 0.0;

    std::ofstream sender_stats(path_join(cfg.out_dir, "sender_stats.csv"));
    sender_stats << SENDER_HEADER << '\n' << state.sent << ',' << state.send_fail << ',' << std::fixed
                 << std::setprecision(6) << send_elapsed_sec << ',' << payload_rate_mbps << ',' << interval_mean << ','
                 << percentile(sorted_intervals, 0.50) << ',' << percentile(sorted_intervals, 0.95) << ','
                 << percentile(sorted_intervals, 0.99) << ',' << state.intervals.maximum << '\n';
    std::cout << "Sender complete: sent=" << state.sent << " failed=" << state.send_fail << '\n';
}

void run_receiver(const Config &cfg)
{
    if (!make_dirs(cfg.out_dir))
        throw std::runtime_error("cannot create output directory " + cfg.out_dir);
    const auto output_dir = absolute_path(cfg.out_dir);
    if (!change_directory(output_dir))
        throw std::runtime_error("cannot enter output directory " + output_dir);
    std::ofstream capture_meta("capture_meta.csv");
    capture_meta << "contract_version,test_kind\n" << CSV_CONTRACT_VERSION << ",network\n";
    capture_meta.close();
    ReceiverState state(SteadyClock::now(), "arrival_intervals.csv", "input_intervals.csv");

    std::shared_ptr<ReliableUDP> udp;
    udp = std::make_shared<ReliableUDP>(BG_SERVICE, cfg.local_port);
    udp->set_observe_callback([&state](SteadyClock::time_point now) { state.note_input(now); });
    udp->set_receive_callback([&state, &udp](const std::vector<QueueFrame> &frames, bool allow_immediate) {
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
            if (!state.remember_sequence(header.seq))
            {
                ++state.duplicates;
                continue;
            }
            const bool synced = udp->is_time_synced();
            const double interval = state.has_last_arrival ? elapsed_ms(state.last_arrival, steady_now) : 0.0;
            const double latency = synced ? (static_cast<double>(wall_time_us()) -
                                             static_cast<double>(header.send_wall_us) +
                                             static_cast<double>(udp->offset_ms()) * 1000.0) / 1000.0 : 0.0;
            state.arrivals << std::fixed << std::setprecision(3) << elapsed_s(state.start, steady_now) << ','
                           << header.seq << ',' << interval << ',';
            if (synced && latency >= 0.0)
                state.arrivals << latency;
            state.arrivals << ',' << frame.size << ',' << (allow_immediate ? 1 : 0) << '\n';
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

    std::cout << "Receiver capture keeps full per-frame CSV data; allow about 450 MB for a 24-hour run.\n";
    const auto first_deadline = SteadyClock::now() + std::chrono::seconds(60);
    SteadyClock::time_point capture_deadline{};
    while (true)
    {
        const auto now = SteadyClock::now();
        if (!state.first_frame && now >= first_deadline)
            throw std::runtime_error("timed out waiting for the first valid frame");
        if (state.first_frame && capture_deadline == SteadyClock::time_point{})
            capture_deadline = now + std::chrono::seconds(cfg.duration_sec + 3);
        if (capture_deadline != SteadyClock::time_point{} && now >= capture_deadline)
            break;

        state.flush_if_due(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool synced = udp->is_time_synced();
    const auto rtt = udp->rtt_ms();
    const auto offset = udp->offset_ms();
    udp->stop();
    state.arrivals.flush();
    state.inputs.flush();

    std::ofstream summary("receiver_summary.txt");
    write_common_summary(summary, cfg);
    summary << "clock_synced=" << (synced ? 1 : 0) << '\n' << "rtt_ms=" << rtt << '\n'
            << "offset_ms=" << offset << '\n' << "received_count=" << state.received << '\n'
            << "integrity_errors=" << state.integrity_errors << '\n'
            << "duplicate_count=" << state.duplicates << '\n';
    std::cout << "Receiver complete: received=" << state.received
              << " errors=" << state.integrity_errors << '\n';
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
