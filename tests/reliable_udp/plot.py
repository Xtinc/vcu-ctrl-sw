#!/usr/bin/env python3
import argparse
import csv
import math
import os
from collections import deque

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.makedirs(os.environ["MPLCONFIGDIR"], exist_ok=True)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SAVE_DPI = 300
ROLLING_WINDOW = 100
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
ARRIVAL_COLUMNS = {"elapsed_ms", "seq", "interval_ms", "latency_ms"}
QUEUE_COLUMNS = {
    "elapsed_ms", "q_short_fi_ms", "q_avg_fi_ms", "q_out_fi_ms", "q_tail_jitter_frames",
    "q_buffered_frames", "q_adaptive_depth", "q_depth_raw", "q_pressure_frames",
    "q_skip_delta", "q_drop_delta", "q_late_delta", "q_reorder_delta",
    "q_stale_delta", "q_ovf_delta",
}
STAGE_COLUMNS = {"stage", "start_s", "end_s", "netem_args"}


def as_float(row, key, default=0.0):
    try:
        value = row.get(key, "")
        return default if value == "" else float(value)
    except (TypeError, ValueError):
        return default


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


class SampleStats:
    """Exact statistics over all captured values."""

    def __init__(self):
        self.values = []
        self.count = 0
        self.total = 0.0
        self.maximum = 0.0

    def add(self, value):
        self.count += 1
        self.total += value
        self.maximum = max(self.maximum, value)
        self.values.append(value)

    def summary(self):
        return {
            "count": self.count,
            "mean": self.total / self.count if self.count else 0.0,
            "p50": percentile(self.values, 0.50),
            "p90": percentile(self.values, 0.90),
            "p95": percentile(self.values, 0.95),
            "p99": percentile(self.values, 0.99),
            "max": self.maximum,
        }


def csv_rows(path, required, include_rotated=False):
    paths = [path]
    if include_rotated:
        archives = []
        index = 1
        while os.path.exists("{}.{}".format(path, index)):
            archives.append("{}.{}".format(path, index))
            index += 1
        paths = list(reversed(archives)) + paths

    for current_path in paths:
        with open(current_path, newline="") as stream:
            reader = csv.DictReader(stream)
            fields = set(reader.fieldnames or ())
            missing = sorted(required - fields)
            if missing:
                raise SystemExit("{} is missing required columns: {}".format(current_path, ", ".join(missing)))
            for row in reader:
                yield row


def read_summary(path):
    values = {}
    if not path or not os.path.exists(path):
        return values
    with open(path) as stream:
        for line in stream:
            key, separator, value = line.strip().partition("=")
            if separator:
                values[key] = value
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
    for row in csv_rows(path, STAGE_COLUMNS):
        start = as_float(row, "start_s")
        end = as_float(row, "end_s")
        if end > start:
            stage = {"stage": row.get("stage", ""), "start_s": start, "end_s": end}
            stage.update(parse_netem_args(row.get("netem_args", "")))
            stages.append(stage)
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


def process_arrivals(path, stages, stage_metrics):
    interval_stats = SampleStats()
    latency_stats = SampleStats()
    cv_stats = SampleStats()
    recent_intervals = deque()
    interval_sum = 0.0
    interval_sum_sq = 0.0
    recent_latencies = deque()
    plot = {"raw_t": [], "raw_v": [], "smooth_t": [], "interval_mean": [], "interval_p95": [],
            "latency_mean": [], "latency_p95": [], "qse_t": [], "qse_v": []}
    last_qse = None
    last_time = 0.0
    row_count = 0

    for row in csv_rows(path, ARRIVAL_COLUMNS):
        row_count += 1
        timestamp = as_float(row, "elapsed_ms") / 1000.0
        last_time = timestamp
        qse = 1 if as_float(row, "qs_immediate") > 0 else 0
        if qse != last_qse:
            plot["qse_t"].append(timestamp)
            plot["qse_v"].append(qse)
            last_qse = qse

        latency = as_float(row, "latency_ms", -1.0)
        if math.isfinite(latency) and latency >= 0.0:
            latency_stats.add(latency)
            recent_latencies.append(latency)
            if len(recent_latencies) > ROLLING_WINDOW:
                recent_latencies.popleft()
            stage_index = stage_for_time(stages, timestamp)
            if stage_index is not None:
                stage_metrics[stage_index].latency.add(latency)

        interval = as_float(row, "interval_ms")
        if interval <= 0.0:
            continue
        interval_stats.add(interval)
        recent_intervals.append(interval)
        interval_sum += interval
        interval_sum_sq += interval * interval
        if len(recent_intervals) > ROLLING_WINDOW:
            removed = recent_intervals.popleft()
            interval_sum -= removed
            interval_sum_sq -= removed * removed
        mean = interval_sum / len(recent_intervals)
        variance = max(0.0, interval_sum_sq / len(recent_intervals) - mean * mean)
        cv_stats.add(math.sqrt(variance) / mean if mean > 0.0 else 0.0)
        plot["raw_t"].append(timestamp)
        plot["raw_v"].append(interval)
        plot["smooth_t"].append(timestamp)
        plot["interval_mean"].append(mean)
        plot["interval_p95"].append(percentile(list(recent_intervals), 0.95))
        plot["latency_mean"].append(sum(recent_latencies) / len(recent_latencies) if recent_latencies else float("nan"))
        plot["latency_p95"].append(percentile(list(recent_latencies), 0.95) if recent_latencies else float("nan"))
    if plot["qse_t"]:
        plot["qse_t"].append(last_time)
        plot["qse_v"].append(plot["qse_v"][-1])
    return {"count": row_count, "plot": plot, "interval": interval_stats.summary(),
            "latency": latency_stats.summary(), "cv": cv_stats.summary()}


def process_queue(path, stages, stage_metrics):
    keys = ("q_short_fi_ms", "q_avg_fi_ms", "q_out_fi_ms", "q_buffered_frames", "q_adaptive_depth",
            "q_skip_delta", "q_drop_delta", "q_reorder_delta", "q_tail_jitter_frames",
            "q_disorder_frames", "q_pressure_frames")
    plot = {"t": []}
    for key in keys:
        plot[key] = []
    input_stats = SampleStats()
    short_input_stats = SampleStats()
    output_stats = SampleStats()
    buffered_stats = SampleStats()
    target_stats = SampleStats()
    events = {"skip": 0.0, "late": 0.0, "overflow": 0.0, "drop": 0.0, "stale": 0.0, "reorder": 0.0}
    gaps = []
    parts = {}
    previous_segment = None
    row_count = 0
    for row in csv_rows(path, QUEUE_COLUMNS, include_rotated=True):
        row_count += 1
        timestamp = as_float(row, "elapsed_ms") / 1000.0
        part_id = int(as_float(row, "part_id", 0.0))
        part = parts.setdefault(part_id, {"part_id": part_id, "start_s": timestamp, "end_s": timestamp, "rows": 0})
        part["end_s"] = timestamp
        part["rows"] += 1
        segment = int(as_float(row, "segment_id", 1.0))
        if previous_segment is not None and segment != previous_segment:
            for key in keys:
                plot[key].append(float("nan"))
            plot["t"].append(timestamp)
            gaps.append({"time": timestamp, "idle_ms": as_float(row, "idle_gap_ms"), "segment": segment})
        previous_segment = segment
        values = {key: as_float(row, key) for key in keys}
        if values["q_avg_fi_ms"] > 0.0:
            input_stats.add(values["q_avg_fi_ms"])
            if values["q_short_fi_ms"] > 0.0:
                short_input_stats.add(values["q_short_fi_ms"])
            output_stats.add(values["q_out_fi_ms"])
            buffered_stats.add(values["q_buffered_frames"])
            target_stats.add(values["q_adaptive_depth"])
        events["skip"] += values["q_skip_delta"]
        events["drop"] += values["q_drop_delta"]
        events["reorder"] += values["q_reorder_delta"]
        events["late"] += as_float(row, "q_late_delta")
        events["stale"] += as_float(row, "q_stale_delta")
        events["overflow"] += as_float(row, "q_ovf_delta")
        stage_index = stage_for_time(stages, timestamp)
        if stage_index is not None:
            metrics = stage_metrics[stage_index]
            metrics.target.add(values["q_adaptive_depth"])
            metrics.raw.add(as_float(row, "q_depth_raw"))
            metrics.reorder_driver.add(as_float(row, "q_disorder_frames"))
            metrics.jitter_driver.add(1.5 * values["q_tail_jitter_frames"])
            metrics.pressure_driver.add(1.0 if values["q_pressure_frames"] > 0.0 else 0.0)
            metrics.reorder_events += values["q_reorder_delta"]
            metrics.bad_events += values["q_skip_delta"] + as_float(row, "q_late_delta") + as_float(row, "q_ovf_delta")
        plot["t"].append(timestamp)
        for key in keys:
            plot[key].append(values[key])
    return {"count": row_count, "plot": plot, "gaps": gaps, "parts": [parts[key] for key in sorted(parts)],
            "input": input_stats.summary(), "short_input": short_input_stats.summary(),
            "output": output_stats.summary(), "buffered": buffered_stats.summary(),
            "target": target_stats.summary(), "events": events}


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


def plot_output(out_dir, arrivals, sender, stages):
    data = arrivals["plot"]
    has_latency = arrivals["latency"]["count"] > 0
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
    save_figure(fig, out_dir, "output_smoothness.png")


def plot_controller(out_dir, queue, stages):
    data = queue["plot"]
    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    axes[0].plot(data["t"], data["q_short_fi_ms"], linewidth=0.9, alpha=0.8,
                 label="short-term input mean (15 intervals)")
    axes[0].plot(data["t"], data["q_avg_fi_ms"], linewidth=1.2,
                 label="smoothed controller input estimate")
    axes[0].plot(data["t"], data["q_out_fi_ms"], linewidth=1.0, label="commanded output interval")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Controller Interval")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")
    axes[1].plot(data["t"], data["q_buffered_frames"], linewidth=1.1, label="actual depth")
    axes[1].plot(data["t"], data["q_adaptive_depth"], linewidth=1.1, label="target depth")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Buffer Depth")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "controller_terms.png")


def plot_network(out_dir, queue, stages):
    data = queue["plot"]
    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    event_lines = (("q_reorder_delta", "reorder events"), ("q_skip_delta", "skipped frames"),
                   ("q_drop_delta", "dropped frames"))
    plotted = False
    for key, label in event_lines:
        if any(value > 0.0 for value in data[key]):
            axes[0].plot(data["t"], data[key], linewidth=1.0, label=label)
            plotted = True
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

    jitter_driver = [1.5 * value for value in data["q_tail_jitter_frames"]]
    axes[1].plot(data["t"], jitter_driver, linewidth=1.1, label="tail jitter driver")
    if any(value > 0.0 for value in data["q_disorder_frames"]):
        axes[1].plot(data["t"], data["q_disorder_frames"], linewidth=0.9, label="reorder driver")
    pressure = [1.0 if value > 0.0 else 0.0 for value in data["q_pressure_frames"]]
    if any(pressure):
        axes[1].plot(data["t"], pressure, linewidth=0.9, label="pressure driver")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Active Controller Drivers")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "network_effects.png")


def stage_rows(stages, metrics):
    rows = []
    for stage, values in zip(stages, metrics):
        rows.append({
            "stage": stage["stage"], "start_s": stage["start_s"], "end_s": stage["end_s"],
            "target_p50_frames": values.target.summary()["p50"],
            "target_p95_frames": values.target.summary()["p95"],
            "raw_p50_frames": values.raw.summary()["p50"],
            "reorder_p95_frames": values.reorder_driver.summary()["p95"],
            "jitter_driver_p95_frames": values.jitter_driver.summary()["p95"],
            "pressure_p95_frames": values.pressure_driver.summary()["p95"],
            "latency_p50_ms": values.latency.summary()["p50"],
            "latency_p95_ms": values.latency.summary()["p95"],
            "reorder_events": values.reorder_events, "bad_events": values.bad_events,
        })
    return rows


def write_assessment(out_dir, arrivals, queue, sender, stages):
    interval = arrivals["interval"]
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
        stream.write("- samples: stats={}, arrivals={}\n\n".format(queue["count"], arrivals["count"]))
        stream.write("CSV parts\npart_id,start_s,end_s,rows\n")
        for part in queue["parts"]:
            stream.write("{part_id},{start_s:.3f},{end_s:.3f},{rows}\n".format(**part))
        stream.write("\n")
        if sender:
            received = arrivals["count"]
            loss = 100.0 * max(0, sender["sent"] - received) / sender["sent"] if sender["sent"] else 0.0
            stream.write("End-to-end\n- sent: {}\n- received: {}\n- send_fail: {}\n- loss_pct: {:.3f}\n"
                         "- payload_rate_mbps: {:.3f}\n\n".format(sender["sent"], received, sender["failed"],
                                                                      loss, sender["rate"]))
        stream.write("Output interval (ms)\n")
        for key in ("mean", "p50", "p90", "p95", "p99", "max"):
            stream.write("- {}: {:.2f}\n".format(key, interval[key]))
        if sender:
            stream.write("- sender_mean: {:.2f}\n- receive_minus_send_mean: {:.2f}\n".format(
                sender["mean"], interval["mean"] - sender["mean"]))
        stream.write("- rolling_cv_p95: {:.4f}\n- see: output_smoothness.png\n\n".format(cv["p95"]))
        stream.write("Latency (ms)\n")
        if latency["count"] == 0:
            stream.write("- unavailable: clock synchronization was not established\n")
        else:
            for key in ("mean", "p50", "p90", "p95", "p99", "max"):
                stream.write("- {}: {:.2f}\n".format(key, latency[key]))
        stream.write("\nController\n- short_input_interval_p95: {:.2f} ms\n"
                     "- smoothed_input_interval_p95: {:.2f} ms\n- commanded_output_p95: {:.2f} ms\n"
                     "- buffered_p95: {:.2f} frames\n- target_p95: {:.2f} frames\n- see: controller_terms.png\n\n".format(
                         queue["short_input"]["p95"], queue["input"]["p95"], queue["output"]["p95"],
                         queue["buffered"]["p95"],
                         queue["target"]["p95"]))
        stream.write("Events\n")
        for key in ("skip", "late", "overflow", "drop", "stale", "reorder"):
            stream.write("- {}: {}\n".format(key, int(events[key])))
        stream.write("- see: network_effects.png\n")
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


def main():
    parser = argparse.ArgumentParser(description="Generate bounded-memory ReliableUDP controller plots.")
    parser.add_argument("out_dir", help="directory containing arrival_intervals.csv and queue_stats.csv")
    parser.add_argument("--sender-summary", help="optional sender_summary.txt")
    args = parser.parse_args()
    arrival_path = os.path.join(args.out_dir, "arrival_intervals.csv")
    queue_path = os.path.join(args.out_dir, "queue_stats.csv")
    if not os.path.exists(arrival_path) or not os.path.exists(queue_path):
        raise SystemExit("missing arrival_intervals.csv or queue_stats.csv in {}".format(args.out_dir))
    stages = read_stages(args.out_dir)
    stage_metrics = [StageMetrics() for _ in stages]
    arrivals = process_arrivals(arrival_path, stages, stage_metrics)
    queue = process_queue(queue_path, stages, stage_metrics)
    summary = read_summary(args.sender_summary)
    sender = None
    if "send_interval_mean_ms" in summary:
        sender = {"mean": float(summary.get("send_interval_mean_ms", 0)),
                  "sent": int(summary.get("sent_count", 0)), "failed": int(summary.get("send_fail_count", 0)),
                  "rate": float(summary.get("payload_rate_mbps", 0))}
    remove_old_outputs(args.out_dir)
    plot_output(args.out_dir, arrivals, sender, stages)
    plot_controller(args.out_dir, queue, stages)
    plot_network(args.out_dir, queue, stages)
    write_assessment(args.out_dir, arrivals, queue, sender, stage_rows(stages, stage_metrics))


if __name__ == "__main__":
    main()
