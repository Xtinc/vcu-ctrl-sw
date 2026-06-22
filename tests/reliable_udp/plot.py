#!/usr/bin/env python3
import argparse
import bisect
import glob
import math
import os
import sys
from array import array
from collections import deque

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.makedirs(os.environ["MPLCONFIGDIR"], exist_ok=True)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT_DIR not in sys.path:
    sys.path.insert(0, ROOT_DIR)
from lib_network import plot_queue_stats as queue_core

SAVE_DPI = 300
ROLLING_WINDOW = 100
DEFAULT_PLOT_WINDOW_SECONDS = 600.0
OUTPUTS = {
    "output_smoothness.png",
    "controller_terms.png",
    "network_effects.png",
    "controller_assessment.txt",
}
PLOT_OUTPUTS = OUTPUTS - {"controller_assessment.txt"}
OBSOLETE_OUTPUTS = {
    "arrival_intervals.png", "latency.png", "queue_timing.png", "queue_depth.png",
    "controller_tracking.png", "controller_assessment.png", "controller_dashboard.png",
    "queue_counters.png", "network_vs_controller.png", "latency_ideal_vs_actual.png",
    "parameter_influence.png",
}
CONTRACT_VERSION = "reliable_udp_test_v1"
META_COLUMNS = ("contract_version", "test_kind")
ARRIVAL_COLUMNS = ("elapsed_s", "seq", "interval_ms", "latency_ms", "size_bytes", "allow_immediate")
INPUT_COLUMNS = ("elapsed_s", "interval_ms")
STAGE_COLUMNS = ("stage", "start_s", "end_s", "netem_args")
SENDER_COLUMNS = ("sent_count", "send_fail_count", "send_elapsed_s", "payload_rate_mbps",
                  "interval_mean_ms", "interval_p50_ms", "interval_p95_ms", "interval_p99_ms",
                  "interval_max_ms")


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = p * (len(ordered) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (pos - lo) * (ordered[hi] - ordered[lo])


def percentile_sorted(ordered, p):
    if len(ordered) == 0:
        return 0.0
    if len(ordered) == 1:
        return ordered[0]
    pos = p * (len(ordered) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (pos - lo) * (ordered[hi] - ordered[lo])


class SampleStats:
    """Exact statistics over all captured values."""

    def __init__(self):
        self.values = array("d")
        self.count = 0
        self.total = 0.0
        self.total_sq = 0.0
        self.maximum = 0.0

    def add(self, value):
        self.count += 1
        self.total += value
        self.total_sq += value * value
        self.maximum = max(self.maximum, value)
        self.values.append(value)

    def summary(self):
        # NumPy keeps the temporary sort buffer compact for multi-million-row logs.
        ordered = np.sort(np.frombuffer(self.values, dtype=np.float64))
        mean = self.total / self.count if self.count else 0.0
        variance = max(0.0, self.total_sq / self.count - mean * mean) if self.count else 0.0
        return {
            "count": self.count,
            "mean": mean,
            "cv": math.sqrt(variance) / mean if mean > 0.0 else 0.0,
            "p50": percentile_sorted(ordered, 0.50),
            "p90": percentile_sorted(ordered, 0.90),
            "p95": percentile_sorted(ordered, 0.95),
            "p99": percentile_sorted(ordered, 0.99),
            "max": self.maximum,
        }


def csv_rows(path, expected):
    try:
        for _, row in queue_core.strict_rows(path, expected):
            yield row
    except queue_core.CSVContractError as error:
        raise SystemExit(str(error))


def finite_number(row, key, path, allow_empty=False):
    value = row[key]
    if allow_empty and value == "":
        return None
    try:
        number = float(value)
    except ValueError:
        raise SystemExit("{} has invalid {}: {!r}".format(path, key, value))
    if not math.isfinite(number):
        raise SystemExit("{} has non-finite {}".format(path, key))
    return number


def validate_capture(out_dir):
    meta_path = os.path.join(out_dir, "capture_meta.csv")
    required = [meta_path, os.path.join(out_dir, "input_intervals.csv"),
                os.path.join(out_dir, "arrival_intervals.csv")]
    missing = [path for path in required if not os.path.exists(path)]
    if missing:
        raise SystemExit("missing required test capture files: {}".format(", ".join(missing)))
    meta_rows = list(csv_rows(meta_path, META_COLUMNS))
    if len(meta_rows) != 1:
        raise SystemExit("{} must contain exactly one data row".format(meta_path))
    meta = meta_rows[0]
    if meta["contract_version"] != CONTRACT_VERSION or meta["test_kind"] not in ("network", "tc"):
        raise SystemExit("{} requires contract_version={} and test_kind network|tc".format(
            meta_path, CONTRACT_VERSION))
    input_path = os.path.join(out_dir, "input_intervals.csv")
    input_rows = iter(csv_rows(input_path, INPUT_COLUMNS))
    try:
        first_input = next(input_rows)
    except StopIteration:
        raise SystemExit("{} contains no data rows".format(input_path))
    first_input_elapsed = finite_number(first_input, "elapsed_s", input_path)
    if first_input_elapsed < 0.0:
        raise SystemExit("{} requires non-negative elapsed_s".format(input_path))
    sender_path = os.path.join(out_dir, "sender_stats.csv")
    stage_path = os.path.join(out_dir, "tc_stages.csv")
    if meta["test_kind"] == "tc" and (not os.path.exists(sender_path) or not os.path.exists(stage_path)):
        raise SystemExit("tc capture requires sender_stats.csv and tc_stages.csv")
    return meta["test_kind"], first_input_elapsed


def read_sender(path):
    if not path or not os.path.exists(path):
        return None
    rows = list(csv_rows(path, SENDER_COLUMNS))
    if len(rows) != 1:
        raise SystemExit("{} must contain exactly one data row".format(path))
    row = rows[0]
    values = {key: finite_number(row, key, path) for key in SENDER_COLUMNS}
    if any(value < 0.0 for value in values.values()):
        raise SystemExit("{} values must be non-negative".format(path))
    if not values["sent_count"].is_integer() or not values["send_fail_count"].is_integer():
        raise SystemExit("{} sent_count and send_fail_count must be integers".format(path))
    values.update({"sent": int(values["sent_count"]), "failed": int(values["send_fail_count"]),
                   "rate": values["payload_rate_mbps"], "mean": values["interval_mean_ms"]})
    return values


def parse_netem_args(args):
    def number(token):
        try:
            if token.endswith("ms"):
                return float(token[:-2])
            if token.endswith("us"):
                return float(token[:-2]) / 1000.0
            if token.endswith("s"):
                return float(token[:-1]) * 1000.0
            if token.endswith("%"):
                return float(token[:-1])
            return float(token)
        except (AttributeError, ValueError):
            return 0.0

    spec = {"delay_ms": 0.0, "jitter_ms": 0.0, "slot_min_ms": 0.0, "slot_max_ms": 0.0,
            "reorder_pct": 0.0, "loss_pct": 0.0}
    tokens = args.split()
    for index, token in enumerate(tokens):
        if token == "delay" and index + 1 < len(tokens):
            spec["delay_ms"] = number(tokens[index + 1])
            if index + 2 < len(tokens):
                spec["jitter_ms"] = number(tokens[index + 2])
        elif token == "slot" and index + 1 < len(tokens):
            spec["slot_min_ms"] = number(tokens[index + 1])
            spec["slot_max_ms"] = spec["slot_min_ms"]
            if index + 2 < len(tokens) and tokens[index + 2] not in ("packets", "bytes"):
                spec["slot_max_ms"] = number(tokens[index + 2])
        elif token == "reorder" and index + 1 < len(tokens):
            spec["reorder_pct"] = number(tokens[index + 1])
        elif token == "loss" and index + 1 < len(tokens):
            spec["loss_pct"] = number(tokens[index + 1])
    spec["ideal_latency_ms"] = spec["delay_ms"] + 2.0 * spec["jitter_ms"] + spec["slot_max_ms"]
    return spec


def read_stages(out_dir):
    path = os.path.join(out_dir, "tc_stages.csv")
    if not os.path.exists(path):
        return []
    stages = []
    previous_end = None
    for row in csv_rows(path, STAGE_COLUMNS):
        start = finite_number(row, "start_s", path)
        end = finite_number(row, "end_s", path)
        if not row["stage"] or start < 0.0 or end <= start or (previous_end is not None and start < previous_end):
            raise SystemExit("{} requires named, ordered, non-overlapping stages".format(path))
        stage = {"stage": row["stage"], "start_s": start, "end_s": end}
        stage.update(parse_netem_args(row["netem_args"]))
        stages.append(stage)
        previous_end = end
    if not stages:
        raise SystemExit("{} contains no stage rows".format(path))
    return stages


def stage_for_time(stages, timestamp):
    for index, stage in enumerate(stages):
        if stage["start_s"] <= timestamp < stage["end_s"]:
            return index
    return None


def stage_color(name):
    return {"normal": "tab:green", "jitter": "tab:orange", "jitter2": "tab:red",
            "reorder": "tab:red"}.get(name, "tab:gray")


def annotate_stages(axes, stages):
    if not stages:
        return
    axis_list = list(axes.flat) if hasattr(axes, "flat") else list(axes)
    for axis in axis_list:
        for stage in stages:
            axis.axvspan(stage["start_s"], stage["end_s"], color=stage_color(stage["stage"]),
                         alpha=0.06, linewidth=0)
    for stage in stages:
        axis_list[0].text(0.5 * (stage["start_s"] + stage["end_s"]), 0.98, stage["stage"],
                          transform=axis_list[0].get_xaxis_transform(), ha="center", va="top",
                          fontsize=9, color=stage_color(stage["stage"]))


class StageMetrics:
    def __init__(self):
        self.target = SampleStats()
        self.raw = SampleStats()
        self.reorder_driver = SampleStats()
        self.jitter_driver = SampleStats()
        self.pressure_driver = SampleStats()
        self.latency = SampleStats()
        self.reorder_events = 0.0
        self.bad_events = 0.0


def empty_arrival_plot():
    return {"raw_t": [], "raw_v": [], "smooth_t": [], "interval_mean": [], "interval_p95": [],
            "latency_mean": [], "latency_p95": [], "qse_t": [], "qse_v": []}


def empty_input_plot():
    return {"raw_t": [], "raw_v": [], "smooth_t": [], "interval_mean": [], "interval_p95": []}


def process_arrivals(path, input_path, stages, stage_metrics, window_seconds, emit_window):
    interval_stats = SampleStats()
    latency_stats = SampleStats()
    cv_stats = SampleStats()
    recent_intervals = deque()
    sorted_intervals = []
    interval_sum = 0.0
    interval_sum_sq = 0.0
    recent_latencies = deque()
    sorted_latencies = []
    latency_sum = 0.0
    plot = empty_arrival_plot()
    last_qse = None
    last_time = 0.0
    row_count = 0
    window_start = None
    input_stats = SampleStats()
    input_row_count = 0
    input_rows = iter(csv_rows(input_path, INPUT_COLUMNS))
    input_pending = next(input_rows, None)
    input_plot = empty_input_plot()
    input_recent = deque()
    input_sorted = []
    input_sum = 0.0
    previous_input_time = None
    previous_arrival_time = None

    def consume_inputs(end_time, include_end=False):
        nonlocal input_pending, input_row_count, input_sum, previous_input_time
        while input_pending is not None:
            timestamp = finite_number(input_pending, "elapsed_s", input_path)
            if timestamp > end_time or (timestamp == end_time and not include_end):
                break
            interval = finite_number(input_pending, "interval_ms", input_path)
            if timestamp < 0.0 or interval < 0.0 or (previous_input_time is not None and
                                                      timestamp < previous_input_time):
                raise SystemExit("{} requires non-negative, monotonic timing values".format(input_path))
            previous_input_time = timestamp
            input_pending = next(input_rows, None)
            input_row_count += 1
            if interval <= 0.0:
                continue
            input_stats.add(interval)
            input_recent.append(interval)
            bisect.insort(input_sorted, interval)
            input_sum += interval
            if len(input_recent) > ROLLING_WINDOW:
                removed = input_recent.popleft()
                input_sum -= removed
                input_sorted.pop(bisect.bisect_left(input_sorted, removed))
            if window_start is not None and timestamp >= window_start:
                input_plot["raw_t"].append(timestamp)
                input_plot["raw_v"].append(interval)
                input_plot["smooth_t"].append(timestamp)
                input_plot["interval_mean"].append(input_sum / len(input_recent))
                input_plot["interval_p95"].append(percentile_sorted(input_sorted, 0.95))

    def flush_window(end_time, final=False):
        if not plot["raw_t"]:
            return
        consume_inputs(end_time, include_end=final)
        if plot["qse_t"]:
            plot["qse_t"].append(end_time)
            plot["qse_v"].append(plot["qse_v"][-1])
        emit_window(window_start, end_time, plot, input_plot)

    for row in csv_rows(path, ARRIVAL_COLUMNS):
        row_count += 1
        timestamp = finite_number(row, "elapsed_s", path)
        interval = finite_number(row, "interval_ms", path)
        latency = finite_number(row, "latency_ms", path, allow_empty=True)
        try:
            sequence = int(row["seq"])
            size = int(row["size_bytes"])
            qse = int(row["allow_immediate"])
        except ValueError:
            raise SystemExit("{} has invalid seq, size_bytes, or allow_immediate".format(path))
        if timestamp < 0.0 or interval < 0.0 or (previous_arrival_time is not None and
                                                  timestamp < previous_arrival_time):
            raise SystemExit("{} requires non-negative, monotonic timing values".format(path))
        if sequence < 0 or size <= 0 or qse not in (0, 1) or (latency is not None and latency < 0.0):
            raise SystemExit("{} has out-of-range arrival data".format(path))
        previous_arrival_time = timestamp
        last_time = timestamp
        row_window_start = math.floor(timestamp / window_seconds) * window_seconds
        if window_start is None:
            window_start = row_window_start
        elif row_window_start != window_start:
            flush_window(window_start + window_seconds)
            plot = empty_arrival_plot()
            input_plot = empty_input_plot()
            window_start = row_window_start
            if last_qse is not None:
                plot["qse_t"].append(window_start)
                plot["qse_v"].append(last_qse)
        if qse != last_qse:
            plot["qse_t"].append(timestamp)
            plot["qse_v"].append(qse)
            last_qse = qse

        if latency is not None:
            latency_stats.add(latency)
            recent_latencies.append(latency)
            bisect.insort(sorted_latencies, latency)
            latency_sum += latency
            if len(recent_latencies) > ROLLING_WINDOW:
                removed = recent_latencies.popleft()
                latency_sum -= removed
                sorted_latencies.pop(bisect.bisect_left(sorted_latencies, removed))
            stage_index = stage_for_time(stages, timestamp)
            if stage_index is not None:
                stage_metrics[stage_index].latency.add(latency)

        if interval <= 0.0:
            continue
        interval_stats.add(interval)
        recent_intervals.append(interval)
        bisect.insort(sorted_intervals, interval)
        interval_sum += interval
        interval_sum_sq += interval * interval
        if len(recent_intervals) > ROLLING_WINDOW:
            removed = recent_intervals.popleft()
            interval_sum -= removed
            interval_sum_sq -= removed * removed
            sorted_intervals.pop(bisect.bisect_left(sorted_intervals, removed))
        mean = interval_sum / len(recent_intervals)
        variance = max(0.0, interval_sum_sq / len(recent_intervals) - mean * mean)
        cv_stats.add(math.sqrt(variance) / mean if mean > 0.0 else 0.0)
        plot["raw_t"].append(timestamp)
        plot["raw_v"].append(interval)
        plot["smooth_t"].append(timestamp)
        plot["interval_mean"].append(mean)
        plot["interval_p95"].append(percentile_sorted(sorted_intervals, 0.95))
        plot["latency_mean"].append(latency_sum / len(recent_latencies) if recent_latencies else float("nan"))
        plot["latency_p95"].append(percentile_sorted(sorted_latencies, 0.95) if recent_latencies else float("nan"))
    if row_count == 0:
        raise SystemExit("{} contains no data rows".format(path))
    if window_start is not None:
        flush_window(last_time, final=True)
    consume_inputs(float("inf"), include_end=True)
    return {"count": row_count, "interval": interval_stats.summary(),
            "latency": latency_stats.summary(), "cv": cv_stats.summary(),
            "input": input_stats.summary(),
            "input_count": input_row_count}


def align_queue(queue, offset_s):
    queue["plot"]["t"] = array("d", (value + offset_s for value in queue["plot"]["t"]))
    for session in queue["sessions"]:
        session["start_s"] += offset_s
        session["end_s"] += offset_s
    for gap in queue["gaps"]:
        gap["time"] += offset_s


def collect_queue_stage_metrics(queue, stages, metrics):
    data = queue["plot"]
    for index, timestamp in enumerate(data["t"]):
        if not math.isfinite(data["q_adaptive_depth"][index]):
            continue
        stage_index = stage_for_time(stages, timestamp)
        if stage_index is None:
            continue
        values = metrics[stage_index]
        values.target.add(data["q_adaptive_depth"][index])
        values.raw.add(data["q_depth_raw"][index])
        values.reorder_driver.add(data["q_disorder_frames"][index])
        values.jitter_driver.add(1.5 * data["q_tail_jitter_frames"][index])
        values.pressure_driver.add(1.0 if data["q_pressure_frames"][index] > 0.0 else 0.0)
        values.reorder_events += data["q_reorder_delta"][index]
        values.bad_events += (data["q_skip_delta"][index] + data["q_late_delta"][index] +
                              data["q_ovf_delta"][index])


def set_robust_upper_ylim(axis, values, p=0.99, margin=1.2):
    valid = [value for value in values if math.isfinite(value) and value >= 0.0]
    if valid:
        upper = max(1.0, percentile(valid, p) * margin)
        axis.set_ylim(bottom=0.0, top=upper if max(valid) > upper else None)


def save_figure(fig, out_dir, filename):
    fig.tight_layout()
    path = os.path.join(out_dir, filename)
    fig.savefig(path, dpi=SAVE_DPI)
    plt.close(fig)
    print("wrote {}".format(path))


def plot_output(out_dir, arrivals, sender, stages, filename, time_range):
    data = arrivals["plot"]
    has_latency = any(math.isfinite(value) for value in data["latency_mean"])
    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    axes[0].plot(data["raw_t"], data["raw_v"], linewidth=0.45, alpha=0.25, label="output interval samples")
    axes[0].plot(data["smooth_t"], data["interval_mean"], linewidth=1.2, label="rolling mean")
    axes[0].plot(data["smooth_t"], data["interval_p95"], linewidth=1.1, label="rolling p95")
    if sender:
        axes[0].axhline(sender["mean"], color="tab:green", linestyle="--", label="sender mean")
    if data["qse_t"]:
        mode_axis = axes[0].twinx()
        mode_axis.step(data["qse_t"], data["qse_v"], where="post", color="tab:purple", alpha=0.55,
                       linewidth=1.0, label="QSE mode")
        mode_axis.set_ylim(-0.1, 1.1)
        mode_axis.set_yticks([0, 1])
        mode_axis.set_yticklabels(["buffered", "immediate"])
        mode_axis.set_ylabel("QSE mode")
        lines, labels = axes[0].get_legend_handles_labels()
        extra_lines, extra_labels = mode_axis.get_legend_handles_labels()
        axes[0].legend(lines + extra_lines, labels + extra_labels, loc="upper right")
    else:
        axes[0].legend(loc="upper right")
    axes[0].set_ylabel("ms")
    axes[0].set_title("User-Visible Output Interval and QSE Mode")
    set_robust_upper_ylim(axes[0], data["raw_v"] + data["interval_p95"])
    axes[0].grid(True, alpha=0.25)

    if has_latency:
        axes[1].plot(data["smooth_t"], data["latency_mean"], linewidth=1.1, label="latency rolling mean")
        axes[1].plot(data["smooth_t"], data["latency_p95"], linewidth=1.1, label="latency rolling p95")
    if stages:
        times, values = [], []
        for stage in stages:
            times.extend([stage["start_s"], stage["end_s"]])
            values.extend([stage["ideal_latency_ms"], stage["ideal_latency_ms"]])
        axes[1].plot(times, values, linestyle="--", linewidth=1.0, label="configured high latency envelope")
    if not has_latency:
        axes[1].text(0.5, 0.5, "Latency unavailable: clocks were not synchronized",
                     transform=axes[1].transAxes, ha="center", va="center", fontsize=12, color="tab:red")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("ms")
    axes[1].set_title("User-Visible Latency")
    axes[1].grid(True, alpha=0.25)
    if has_latency or stages:
        axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    axes[1].set_xlim(*time_range)
    save_figure(fig, out_dir, filename)


def plot_controller(out_dir, queue, arrivals, inputs, stages, filename, time_range):
    data = queue["plot"]
    output = arrivals["plot"]
    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    queue_core.plot_controller_axes(axes, queue)
    axes[0].plot(inputs["raw_t"], inputs["raw_v"], color="tab:red", linewidth=0.45, alpha=0.20,
                 label="controller input interval (per frame)")
    axes[0].plot(inputs["smooth_t"], inputs["interval_mean"], color="tab:red", linewidth=1.2,
                 label="controller input rolling mean")
    axes[0].plot(inputs["smooth_t"], inputs["interval_p95"], color="tab:orange", linewidth=1.0,
                 label="controller input rolling p95")
    input_upper = max(inputs["interval_p95"], default=0.0)
    if inputs["raw_v"]:
        input_upper = max(input_upper, percentile(inputs["raw_v"], 0.99))
    axes[0].plot(output["raw_t"], output["raw_v"], color="tab:blue", linewidth=0.45, alpha=0.25,
                 label="controlled output interval (per frame)")
    axes[0].plot(output["smooth_t"], output["interval_mean"], color="tab:blue", linewidth=1.2,
                 label="controlled output rolling mean")
    axes[0].plot(output["smooth_t"], output["interval_p95"], color="tab:cyan", linewidth=1.0,
                 label="controlled output rolling p95")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Network-Affected Input vs Controlled Output")
    output_upper = max(output["interval_p95"], default=0.0)
    if output["raw_v"]:
        output_upper = max(output_upper, percentile(output["raw_v"], 0.99))
    product_upper = max((value for key in ("q_short_fi_ms", "q_avg_fi_ms", "q_out_fi_ms")
                         for value in data[key] if math.isfinite(value)), default=0.0)
    axes[0].set_ylim(bottom=0.0, top=max(1.0, input_upper, output_upper, product_upper) * 1.08)
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Buffer Depth")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    axes[1].set_xlim(*time_range)
    save_figure(fig, out_dir, filename)


def plot_network(out_dir, queue, stages, filename, time_range):
    data = queue["plot"]
    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    queue_core.plot_network_axes(axes, queue)
    event_lines = (("q_reorder_delta", "reorder events"), ("q_skip_delta", "skipped frames"),
                   ("q_drop_delta", "dropped frames"))
    plotted = False
    for key, _ in event_lines:
        plotted = plotted or any(value > 0.0 for value in data[key])
    if stages:
        parameter_axis = axes[0].twinx()
        for key, label, color in (("reorder_pct", "configured reorder", "tab:purple"),
                                  ("loss_pct", "configured loss", "tab:brown")):
            times, values = [], []
            for stage in stages:
                times.extend([stage["start_s"], stage["end_s"]])
                values.extend([stage[key], stage[key]])
            if any(value > 0.0 for value in values):
                parameter_axis.plot(times, values, linestyle="--", linewidth=0.9, color=color, label=label)
        lines, labels = axes[0].get_legend_handles_labels()
        extra, extra_labels = parameter_axis.get_legend_handles_labels()
        if lines or extra:
            axes[0].legend(lines + extra, labels + extra_labels, loc="upper right")
    elif plotted:
        axes[0].legend(loc="upper right")
    if not plotted and not stages:
        axes[0].text(0.5, 0.5, "No queue/network events", transform=axes[0].transAxes,
                     ha="center", va="center", color="tab:green")
    axes[0].set_ylabel("count / stats period")
    axes[0].set_title("Observed Queue and Network Events")
    axes[0].grid(True, alpha=0.25)

    pressure = [1.0 if value > 0.0 else 0.0 for value in data["q_pressure_frames"]]
    if any(pressure):
        axes[1].plot(data["t"], pressure, linewidth=0.9, label="pressure driver")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Active Controller Drivers")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    axes[1].set_xlim(*time_range)
    save_figure(fig, out_dir, filename)


def stage_rows(stages, metrics):
    rows = []
    for stage, values in zip(stages, metrics):
        target = values.target.summary()
        raw = values.raw.summary()
        reorder = values.reorder_driver.summary()
        jitter = values.jitter_driver.summary()
        pressure = values.pressure_driver.summary()
        latency = values.latency.summary()
        rows.append({
            "stage": stage["stage"], "start_s": stage["start_s"], "end_s": stage["end_s"],
            "target_p50_frames": target["p50"],
            "target_p95_frames": target["p95"],
            "raw_p50_frames": raw["p50"],
            "reorder_p95_frames": reorder["p95"],
            "jitter_driver_p95_frames": jitter["p95"],
            "pressure_p95_frames": pressure["p95"],
            "latency_p50_ms": latency["p50"],
            "latency_p95_ms": latency["p95"],
            "reorder_events": values.reorder_events, "bad_events": values.bad_events,
        })
    return rows


def write_assessment(out_dir, arrivals, queue, sender, stages):
    interval = arrivals["interval"]
    input_interval = arrivals["input"]
    latency = arrivals["latency"]
    cv = arrivals["cv"]
    events = queue["events"]
    visible_failures = events["skip"] + events["late"] + events["overflow"]
    score = 100.0
    score -= min(45.0, cv["p95"] * 50.0)
    score -= min(30.0, visible_failures * 10.0)
    score -= min(15.0, max(0.0, interval["p99"] - interval["p50"]) * 0.5)
    score = max(0.0, min(100.0, score))
    label = "GOOD" if score >= 90 else "WATCH" if score >= 70 else "BAD"
    path = os.path.join(out_dir, "controller_assessment.txt")
    with open(path, "w") as stream:
        stream.write("ReliableUDP output smoothness assessment\n========================================\n\n")
        stream.write("Overall\n- score: {} / 100 ({})\n".format(int(round(score)), label))
        stream.write("- percentile_note: computed from all samples\n")
        input_count = arrivals["input_count"]
        stream.write("- samples: stats={}, inputs={}, outputs={}\n\n".format(
            queue["count"], input_count, arrivals["count"]))
        stream.write("Queue sessions\nsession_id,start_utc,start_s,end_s,rows\n")
        for session in queue["sessions"]:
            stream.write("{session_id},{start_utc},{start_s:.3f},{end_s:.3f},{rows}\n".format(**session))
        stream.write("\n")
        if sender:
            received = arrivals["count"]
            loss = 100.0 * max(0, sender["sent"] - received) / sender["sent"] if sender["sent"] else 0.0
            stream.write("End-to-end\n- sent: {}\n- received: {}\n- send_fail: {}\n- loss_pct: {:.3f}\n"
                         "- sender_elapsed_s: {:.3f}\n"
                         "- payload_rate_mbps: {:.3f}\n- sender_interval_p50_ms: {:.3f}\n"
                         "- sender_interval_p95_ms: {:.3f}\n- sender_interval_p99_ms: {:.3f}\n"
                         "- sender_interval_max_ms: {:.3f}\n\n".format(
                             sender["sent"], received, sender["failed"], loss, sender["send_elapsed_s"], sender["rate"],
                             sender["interval_p50_ms"], sender["interval_p95_ms"],
                             sender["interval_p99_ms"], sender["interval_max_ms"]))
        stream.write("Input/output jitter comparison\n")
        for label, values in (("input", input_interval), ("output", interval)):
            stream.write("- {}_mean_ms: {:.3f}\n".format(label, values["mean"]))
            stream.write("- {}_cv: {:.6f}\n".format(label, values["cv"]))
            stream.write("- {}_p95_ms: {:.3f}\n".format(label, values["p95"]))
            stream.write("- {}_p99_ms: {:.3f}\n".format(label, values["p99"]))
            stream.write("- {}_max_ms: {:.3f}\n".format(label, values["max"]))
        if input_interval["cv"] > 0.0:
            reduction = (1.0 - interval["cv"] / input_interval["cv"]) * 100.0
            stream.write("- cv_reduction_pct: {:.2f}\n".format(reduction))
        else:
            stream.write("- cv_reduction_pct: unavailable\n")
        stream.write("- see: controller_terms_*.png\n\n")
        stream.write("Output interval (ms)\n")
        for key in ("mean", "p50", "p90", "p95", "p99", "max"):
            stream.write("- {}: {:.2f}\n".format(key, interval[key]))
        if sender:
            stream.write("- sender_mean: {:.2f}\n- receive_minus_send_mean: {:.2f}\n".format(
                sender["mean"], interval["mean"] - sender["mean"]))
        stream.write("- rolling_cv_p95: {:.4f}\n- see: output_smoothness_*.png\n\n".format(cv["p95"]))
        stream.write("Latency (ms)\n")
        if latency["count"] == 0:
            stream.write("- unavailable: clock synchronization was not established\n")
        else:
            for key in ("mean", "p50", "p90", "p95", "p99", "max"):
                stream.write("- {}: {:.2f}\n".format(key, latency[key]))
        stream.write("\nController\n- short_input_interval_p95: {:.2f} ms\n"
                     "- smoothed_input_interval_p95: {:.2f} ms\n- commanded_output_p95: {:.2f} ms\n"
                     "- buffered_p95: {:.2f} frames\n- target_p95: {:.2f} frames\n- see: controller_terms_*.png\n\n".format(
                         queue["short_input"]["p95"], queue["input"]["p95"], queue["output"]["p95"],
                         queue["buffered"]["p95"],
                         queue["target"]["p95"]))
        stream.write("Events\n")
        for key in ("skip", "late", "overflow", "drop", "stale", "reorder"):
            stream.write("- {}: {}\n".format(key, int(events[key])))
        stream.write("- see: network_effects_*.png\n")
        if stages:
            stream.write("\nStage effects\nstage,start_s,end_s,target_p50_frames,target_p95_frames,raw_p50_frames,"
                         "reorder_p95_frames,jitter_driver_p95_frames,pressure_p95_frames,latency_p50_ms,"
                         "latency_p95_ms,reorder_events,bad_events\n")
            for row in stages:
                stream.write("{stage},{start_s:.0f},{end_s:.0f},{target_p50_frames:.2f},{target_p95_frames:.2f},"
                             "{raw_p50_frames:.2f},{reorder_p95_frames:.2f},{jitter_driver_p95_frames:.2f},"
                             "{pressure_p95_frames:.2f},{latency_p50_ms:.2f},{latency_p95_ms:.2f},"
                             "{reorder_events:.0f},{bad_events:.0f}\n".format(**row))
    print("wrote {}".format(path))


def remove_old_outputs(out_dir):
    for name in OBSOLETE_OUTPUTS | OUTPUTS:
        path = os.path.join(out_dir, name)
        if os.path.exists(path):
            os.remove(path)
    for prefix in ("output_smoothness_", "controller_terms_", "network_effects_"):
        for path in glob.glob(os.path.join(out_dir, prefix + "*.png")):
            os.remove(path)


def main():
    parser = argparse.ArgumentParser(description="Generate bounded-memory ReliableUDP controller plots.")
    parser.add_argument(
        "out_dir", help="test output containing input_intervals.csv, arrival_intervals.csv, and queue_stats.csv")
    parser.add_argument("--sender-stats", help="optional sender_stats.csv from a separate sender")
    parser.add_argument("--plot-window-seconds", type=float, default=DEFAULT_PLOT_WINDOW_SECONDS,
                        help="seconds shown in each full-resolution plot set (default: %(default)s)")
    args = parser.parse_args()
    if not math.isfinite(args.plot_window_seconds) or args.plot_window_seconds <= 0.0:
        parser.error("--plot-window-seconds must be greater than zero")
    arrival_path = os.path.join(args.out_dir, "arrival_intervals.csv")
    input_path = os.path.join(args.out_dir, "input_intervals.csv")
    queue_path = os.path.join(args.out_dir, "queue_stats.csv")
    if not os.path.exists(arrival_path) or not os.path.exists(input_path) or not os.path.exists(queue_path):
        raise SystemExit("missing arrival_intervals.csv, input_intervals.csv, or queue_stats.csv in {}".format(
            args.out_dir))
    test_kind, first_input_elapsed = validate_capture(args.out_dir)
    stages = read_stages(args.out_dir)
    stage_metrics = [StageMetrics() for _ in stages]
    sender_path = args.sender_stats or os.path.join(args.out_dir, "sender_stats.csv")
    sender = read_sender(sender_path)
    if test_kind == "tc" and sender is None:
        raise SystemExit("tc capture requires sender_stats.csv")
    remove_old_outputs(args.out_dir)
    try:
        queue = queue_core.process_queue(queue_path)
    except queue_core.CSVContractError as error:
        raise SystemExit(str(error))
    align_queue(queue, first_input_elapsed)
    collect_queue_stage_metrics(queue, stages, stage_metrics)
    page = 0

    def emit_window(start_s, end_s, arrival_plot, input_plot):
        nonlocal page
        page += 1
        suffix = "_{:03d}_{:07.0f}-{:07.0f}s.png".format(page, start_s, end_s)
        time_range = (start_s, end_s)
        window_stages = [stage for stage in stages
                         if stage["end_s"] > start_s and stage["start_s"] < end_s]
        window_arrivals = {"plot": arrival_plot}
        window_queue = dict(queue)
        window_queue["plot"] = queue_core.queue_plot_window(queue, start_s, end_s)
        plot_output(args.out_dir, window_arrivals, sender, window_stages,
                    "output_smoothness" + suffix, time_range)
        plot_controller(args.out_dir, window_queue, window_arrivals, input_plot, window_stages,
                        "controller_terms" + suffix, time_range)
        plot_network(args.out_dir, window_queue, window_stages, "network_effects" + suffix, time_range)

    arrivals = process_arrivals(arrival_path, input_path, stages, stage_metrics,
                                args.plot_window_seconds, emit_window)
    write_assessment(args.out_dir, arrivals, queue, sender, stage_rows(stages, stage_metrics))


if __name__ == "__main__":
    main()
