#!/usr/bin/env python3
"""Strict product-side parser and plots for ReliableUDP queue statistics."""

import argparse
import bisect
import csv
import math
import os
from array import array
from datetime import datetime, timezone

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.makedirs(os.environ["MPLCONFIGDIR"], exist_ok=True)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
QUEUE_HEADER = (
    "timestamp_utc", "session_id", "segment_id", "idle_gap_s", "q_short_fi_ms", "q_avg_fi_ms",
    "q_out_fi_ms", "q_jitter_ms", "q_disorder_frames", "q_max_disorder_depth", "q_tail_jitter_ms",
    "q_buffered_frames", "q_adaptive_depth", "q_depth_raw", "q_pressure_frames", "q_recv_delta",
    "q_dlv_delta", "q_skip_delta", "q_drop_delta", "q_dup_delta", "q_late_delta", "q_reorder_delta",
    "q_stale_delta", "q_ovf_delta",
)

PLOT_KEYS = (
    "q_short_fi_ms", "q_avg_fi_ms", "q_out_fi_ms", "q_buffered_frames", "q_adaptive_depth",
    "q_skip_delta", "q_drop_delta", "q_reorder_delta", "q_tail_jitter_ms", "q_tail_jitter_frames",
    "q_disorder_frames", "q_depth_raw", "q_pressure_frames", "q_late_delta", "q_ovf_delta",
)


class CSVContractError(ValueError):
    pass


def queue_paths(path):
    archives = []
    index = 1
    while os.path.exists("{}.{}".format(path, index)):
        archives.append("{}.{}".format(path, index))
        index += 1
    paths = list(reversed(archives))
    if os.path.exists(path):
        paths.append(path)
    if not paths:
        raise CSVContractError("missing queue statistics: {}".format(path))
    return paths


def strict_rows(path, expected_header):
    with open(path, newline="") as stream:
        reader = csv.reader(stream)
        try:
            header = tuple(next(reader))
        except StopIteration:
            raise CSVContractError("{} is empty".format(path))
        if header != tuple(expected_header):
            raise CSVContractError("{} header mismatch\nexpected: {}\nactual:   {}".format(
                path, ",".join(expected_header), ",".join(header)))
        for line_number, values in enumerate(reader, 2):
            if len(values) != len(expected_header):
                raise CSVContractError("{}:{} expected {} fields, got {}".format(
                    path, line_number, len(expected_header), len(values)))
            yield line_number, dict(zip(expected_header, values))


def _number(row, key, path, line_number):
    try:
        value = float(row[key])
    except ValueError:
        raise CSVContractError("{}:{} invalid {}: {!r}".format(path, line_number, key, row[key]))
    if not math.isfinite(value):
        raise CSVContractError("{}:{} non-finite {}".format(path, line_number, key))
    return value


def _timestamp(value, path, line_number):
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc)
    except ValueError:
        raise CSVContractError("{}:{} invalid timestamp_utc: {!r}".format(path, line_number, value))
    return parsed.timestamp()


def iter_queue_rows(path):
    last_timestamp = None
    last_session = None
    seen_sessions = set()
    row_count = 0
    for current_path in queue_paths(path):
        for line_number, row in strict_rows(current_path, QUEUE_HEADER):
            timestamp = _timestamp(row["timestamp_utc"], current_path, line_number)
            try:
                session_id = int(row["session_id"])
                segment_id = int(row["segment_id"])
            except ValueError:
                raise CSVContractError("{}:{} session_id and segment_id must be integers".format(
                    current_path, line_number))
            if session_id <= 0 or segment_id <= 0:
                raise CSVContractError("{}:{} session_id and segment_id must be positive".format(
                    current_path, line_number))
            if last_timestamp is not None and timestamp < last_timestamp:
                raise CSVContractError("{}:{} timestamp_utc is not monotonic across archives".format(
                    current_path, line_number))
            if session_id != last_session:
                if session_id in seen_sessions:
                    raise CSVContractError("{}:{} session_id reappeared after another session".format(
                        current_path, line_number))
                seen_sessions.add(session_id)
                last_session = session_id
            numeric = {key: _number(row, key, current_path, line_number)
                       for key in QUEUE_HEADER[3:]}
            row_count += 1
            yield {"timestamp": timestamp, "timestamp_utc": row["timestamp_utc"],
                   "session_id": session_id, "segment_id": segment_id, **numeric}
            last_timestamp = timestamp
    if row_count == 0:
        raise CSVContractError("queue statistics contain no data rows")


def summarize(values, active=None):
    selected = [value for index, value in enumerate(values)
                if math.isfinite(value) and (active is None or active[index] > 0.0)]
    if not selected:
        return {"count": 0, "mean": 0.0, "cv": 0.0, "p50": 0.0, "p90": 0.0,
                "p95": 0.0, "p99": 0.0, "max": 0.0}
    total = sum(selected)
    mean = total / len(selected)
    variance = max(0.0, sum(value * value for value in selected) / len(selected) - mean * mean)
    selected.sort()

    def percentile(quantile):
        position = quantile * (len(selected) - 1)
        lower = int(position)
        upper = min(lower + 1, len(selected) - 1)
        return selected[lower] + (position - lower) * (selected[upper] - selected[lower])

    return {"count": len(selected), "mean": mean, "cv": math.sqrt(variance) / mean if mean > 0 else 0.0,
            "p50": percentile(0.50), "p90": percentile(0.90), "p95": percentile(0.95),
            "p99": percentile(0.99), "max": selected[-1]}


def process_queue(path):
    origin = None
    plot = {"t": array("d")}
    for key in PLOT_KEYS:
        plot[key] = array("d")
    events = {name: 0.0 for name in ("skip", "late", "overflow", "drop", "stale", "reorder")}
    gaps = []
    sessions = []
    previous_key = None
    current_session = None
    row_count = 0
    for row in iter_queue_rows(path):
        if origin is None:
            origin = row["timestamp"]
        row_count += 1
        timestamp = row["timestamp"] - origin
        if current_session is None or current_session["session_id"] != row["session_id"]:
            current_session = {"session_id": row["session_id"], "start_s": timestamp,
                               "end_s": timestamp, "rows": 0, "start_utc": row["timestamp_utc"]}
            sessions.append(current_session)
        current_session["end_s"] = timestamp
        current_session["rows"] += 1
        key = (row["session_id"], row["segment_id"])
        if previous_key is not None and key != previous_key:
            for plot_key in PLOT_KEYS:
                plot[plot_key].append(float("nan"))
            plot["t"].append(timestamp)
            gaps.append({"time": timestamp, "idle_s": row["idle_gap_s"], "session_id": row["session_id"],
                         "segment_id": row["segment_id"]})
        previous_key = key
        values = {key: row[key] for key in PLOT_KEYS if key != "q_tail_jitter_frames"}
        interval = row["q_avg_fi_ms"]
        values["q_tail_jitter_frames"] = row["q_tail_jitter_ms"] / interval if interval > 0 else 0.0
        for event, column in (("skip", "q_skip_delta"), ("drop", "q_drop_delta"),
                              ("reorder", "q_reorder_delta"), ("late", "q_late_delta"),
                              ("stale", "q_stale_delta"), ("overflow", "q_ovf_delta")):
            events[event] += row[column]
        plot["t"].append(timestamp)
        for plot_key in PLOT_KEYS:
            plot[plot_key].append(values[plot_key])
    active = plot["q_avg_fi_ms"]
    return {"count": row_count, "plot": plot, "gaps": gaps, "sessions": sessions,
            "input": summarize(plot["q_avg_fi_ms"], plot["q_avg_fi_ms"]),
            "short_input": summarize(plot["q_short_fi_ms"], plot["q_short_fi_ms"]),
            "output": summarize(plot["q_out_fi_ms"], active),
            "buffered": summarize(plot["q_buffered_frames"], active),
            "target": summarize(plot["q_adaptive_depth"], active), "events": events}


def queue_plot_window(queue, start_s, end_s):
    data = queue["plot"]
    first = bisect.bisect_left(data["t"], start_s)
    last = bisect.bisect_right(data["t"], end_s)
    return {key: values[first:last] for key, values in data.items()}


def plot_controller_axes(axes, queue):
    data = queue["plot"]
    axes[0].plot(data["t"], data["q_short_fi_ms"], linewidth=0.8, label="short input interval")
    axes[0].plot(data["t"], data["q_avg_fi_ms"], linewidth=1.1, label="smoothed input interval")
    axes[0].plot(data["t"], data["q_out_fi_ms"], linewidth=1.1, label="commanded output interval")
    axes[0].set_ylabel("ms")
    axes[0].set_title("ReliableUDP Controller Timing")
    axes[1].plot(data["t"], data["q_buffered_frames"], linewidth=1.1, label="actual depth")
    axes[1].plot(data["t"], data["q_adaptive_depth"], linewidth=1.1, label="target depth")
    axes[1].set_ylabel("frames")
    axes[1].set_title("ReliableUDP Buffer Depth")
    for axis in axes:
        axis.grid(True, alpha=0.25)
        axis.legend(loc="upper right")


def plot_network_axes(axes, queue):
    data = queue["plot"]
    for key, label in (("q_reorder_delta", "reorder"), ("q_skip_delta", "skip"), ("q_drop_delta", "drop")):
        axes[0].plot(data["t"], data[key], linewidth=0.9, label=label)
    jitter = [1.5 * value for value in data["q_tail_jitter_frames"]]
    axes[1].plot(data["t"], jitter, linewidth=1.1, label="tail jitter driver")
    axes[1].plot(data["t"], data["q_disorder_frames"], linewidth=0.9, label="reorder driver")
    axes[0].set_title("ReliableUDP Queue Events")
    axes[0].set_ylabel("count / sample")
    axes[1].set_title("ReliableUDP Controller Drivers")
    axes[1].set_ylabel("frames")
    for axis in axes:
        axis.grid(True, alpha=0.25)
        axis.legend(loc="upper right")


def write_core_report(queue, path):
    with open(path, "w") as stream:
        stream.write("ReliableUDP queue statistics\n============================\n")
        stream.write("rows={}\nsessions={}\n\n".format(queue["count"], len(queue["sessions"])))
        for session in queue["sessions"]:
            stream.write("session_id={session_id} start_utc={start_utc} rows={rows} start_s={start_s:.6f} "
                         "end_s={end_s:.6f}\n".format(**session))
        stream.write("\nController\n")
        for name in ("short_input", "input", "output", "buffered", "target"):
            values = queue[name]
            stream.write("{}_mean={:.6f} {}_p95={:.6f} {}_max={:.6f}\n".format(
                name, values["mean"], name, values["p95"], name, values["max"]))
        stream.write("\nEvents\n")
        for key, value in queue["events"].items():
            stream.write("{}={}\n".format(key, int(value)))


def main():
    parser = argparse.ArgumentParser(description="Analyze ReliableUDP product queue statistics")
    parser.add_argument("out_dir", help="directory containing queue_stats.csv and optional rotated files")
    args = parser.parse_args()
    queue = process_queue(os.path.join(args.out_dir, "queue_stats.csv"))
    for name, plotter in (("queue_controller.png", plot_controller_axes), ("queue_network.png", plot_network_axes)):
        fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
        plotter(axes, queue)
        axes[-1].set_xlabel("time since first retained sample (s)")
        fig.tight_layout()
        fig.savefig(os.path.join(args.out_dir, name), dpi=200)
        plt.close(fig)
    write_core_report(queue, os.path.join(args.out_dir, "queue_report.txt"))


if __name__ == "__main__":
    main()
