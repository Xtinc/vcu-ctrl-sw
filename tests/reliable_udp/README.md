# ReliableUDP tests

This directory contains the cross-platform jitter/controller tests and the
Linux-only `tc/netem` loopback benchmark.

## Build configurations

Linux builds the complete hardware, codec, application, and network project:

```sh
cmake -S . -B build
cmake --build build
```

Windows builds only RTOS, ReliableUDP, and the network tests:

```sh
cmake -S . -B build
cmake --build build --target test_udp_network
```

On Windows, run the generated EXE directly. No shell or `tc` runner is used.

## Two-host real-network test

Allow bidirectional UDP traffic for ports 15301 and 15302. Start the receiver
first. Replace the example addresses with addresses reachable from the peer.

Receiver, host B (`192.0.2.20`):

```text
test_udp_network --role receiver --local-port 15302 --peer-address 192.0.2.10 --peer-port 15301 --duration 30 --out-dir receiver_out
```

Sender, host A (`192.0.2.10`):

```text
test_udp_network --role sender --local-port 15301 --peer-address 192.0.2.20 --peer-port 15302 --duration 30 --rate-mbps 5 --payload-bytes 1200 --out-dir sender_out
```

The receiver waits up to 60 seconds for the first valid frame and captures for
the requested duration plus a three-second drain period. Both endpoints set a
destination so the built-in RTT/clock-offset probes work in both directions.
One-way latency is approximate; samples collected before clock synchronization
are marked invalid and excluded by the analyzer.

Receiver-side `ReliableUDP` writes `queue_stats.csv` in the process working
directory. A row is emitted on the receive path roughly every 100 ms without a
timer or an additional thread. `timestamp_utc` is derived from one system/steady
clock anchor captured at startup, and `session_id` separates process runs. Gaps
longer than five seconds start a new `segment_id`; `idle_gap_s` records the gap.
The active file rotates at 64 MiB into `queue_stats.csv.1` through
`queue_stats.csv.3`, bounding retained data to approximately 256 MiB. Starting
a new process archives the previous active file instead of deleting history.

The test receiver also enables the optional controller-input observer and
writes `input_intervals.csv`. Production users do not enable this observer;
their normal runtime output remains only `queue_stats.csv`.

The product analyzer needs only queue statistics:

```sh
python3 lib_network/plot_queue_stats.py receiver_out
```

Test captures additionally have a strict `reliable_udp_test_v1` contract in
`capture_meta.csv`, plus `input_intervals.csv` and `arrival_intervals.csv`.
Copy `sender_out/sender_stats.csv` to the analysis host, then run:

```sh
python3 tests/reliable_udp/plot.py receiver_out --sender-stats sender_stats.csv
```

Without `--sender-stats`, controller plots and receiver-side metrics are
still generated, but end-to-end message loss cannot be calculated.
The sender stats file also contains accepted-send interval mean/p50/p95/p99/max
and achieved payload rate. When supplied, the report and smoothness plot
compare these sender intervals with receiver delivery intervals.

## Linux-only local tc test

The runner requires root or `CAP_NET_ADMIN` and is not configured on Windows:

```sh
sudo tests/reliable_udp/run_tc.sh --preset reorder --duration 30
```

Use `--preset staged --stage-file <path>` for a custom stage sequence. The
default stage file is `stages.txt` in this directory.
