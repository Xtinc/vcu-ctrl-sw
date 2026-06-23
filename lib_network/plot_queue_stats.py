#!/usr/bin/env python3
"""Strict product-side parser and plots for ReliableUDP queue statistics."""

import argparse
import bisect
import csv
import glob
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
    "q_short_fi_ms", "q_avg_fi_ms", "q_out_fi_ms", "q_jitter_ms", "q_tail_jitter_ms",
    "q_tail_jitter_frames", "q_buffered_frames", "q_adaptive_depth", "q_depth_raw", "q_pressure_frames",
    "q_disorder_frames", "q_max_disorder_depth", "q_recv_delta", "q_dlv_delta", "q_skip_delta",
    "q_drop_delta", "q_dup_delta", "q_late_delta", "q_reorder_delta", "q_stale_delta", "q_ovf_delta",
    "estimated_current_delay_ms", "estimated_target_delay_ms", "estimated_raw_delay_ms",
)

DERIVED_KEYS = {
    "q_tail_jitter_frames", "estimated_current_delay_ms", "estimated_target_delay_ms", "estimated_raw_delay_ms"
}


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
    events = {name: 0.0 for name in ("recv", "dlv", "skip", "drop", "dup", "late", "reorder", "stale",
                                                "overflow")}
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
            if previous_key[0] == row["session_id"]:
                gaps.append({"time": timestamp, "idle_s": row["idle_gap_s"], "session_id": row["session_id"],
                             "segment_id": row["segment_id"]})
        previous_key = key
        values = {key: row[key] for key in PLOT_KEYS if key not in DERIVED_KEYS}
        interval = row["q_avg_fi_ms"]
        values["q_tail_jitter_frames"] = row["q_tail_jitter_ms"] / interval if interval > 0 else 0.0
        values["estimated_current_delay_ms"] = row["q_buffered_frames"] * row["q_out_fi_ms"]
        values["estimated_target_delay_ms"] = row["q_adaptive_depth"] * row["q_out_fi_ms"]
        values["estimated_raw_delay_ms"] = row["q_depth_raw"] * row["q_out_fi_ms"]
        for event, column in (("recv", "q_recv_delta"), ("dlv", "q_dlv_delta"),
                              ("skip", "q_skip_delta"), ("drop", "q_drop_delta"),
                              ("dup", "q_dup_delta"), ("late", "q_late_delta"),
                              ("reorder", "q_reorder_delta"), ("stale", "q_stale_delta"),
                              ("overflow", "q_ovf_delta")):
            events[event] += row[column]
        plot["t"].append(timestamp)
        for plot_key in PLOT_KEYS:
            plot[plot_key].append(values[plot_key])
    active = plot["q_avg_fi_ms"]
    pressure_samples = [value for value in plot["q_pressure_frames"] if math.isfinite(value)]
    pressure_ratio = (sum(value > 0.0 for value in pressure_samples) / len(pressure_samples)
                      if pressure_samples else 0.0)
    return {"count": row_count, "plot": plot, "gaps": gaps, "sessions": sessions,
            "input": summarize(plot["q_avg_fi_ms"], plot["q_avg_fi_ms"]),
            "short_input": summarize(plot["q_short_fi_ms"], plot["q_short_fi_ms"]),
            "output": summarize(plot["q_out_fi_ms"], active),
            "jitter": summarize(plot["q_jitter_ms"], active),
            "tail_jitter": summarize(plot["q_tail_jitter_ms"], active),
            "tail_jitter_frames": summarize(plot["q_tail_jitter_frames"], active),
            "disorder": summarize(plot["q_disorder_frames"], active),
            "max_disorder": summarize(plot["q_max_disorder_depth"], active),
            "buffered": summarize(plot["q_buffered_frames"], active),
            "target": summarize(plot["q_adaptive_depth"], active),
            "raw_depth": summarize(plot["q_depth_raw"], active),
            "estimated_delay": {
                "current": summarize(plot["estimated_current_delay_ms"], active),
                "target": summarize(plot["estimated_target_delay_ms"], active),
                "raw": summarize(plot["estimated_raw_delay_ms"], active),
            },
            "pressure_ratio": pressure_ratio, "events": events, "counters": events}


def queue_plot_window(queue, start_s, end_s):
    data = queue["plot"]
    first = bisect.bisect_left(data["t"], start_s)
    last = (bisect.bisect_right(data["t"], end_s) if end_s >= data["t"][-1]
            else bisect.bisect_left(data["t"], end_s))
    return {key: values[first:last] for key, values in data.items()}


def window_ranges(queue, window_seconds):
    timestamps = [value for value in queue["plot"]["t"] if math.isfinite(value)]
    buckets = sorted({int(value // window_seconds) for value in timestamps})
    return [(bucket * window_seconds, (bucket + 1) * window_seconds) for bucket in buckets]


def _style_axes(axes):
    for axis in axes:
        axis.grid(True, alpha=0.25)
        handles, _ = axis.get_legend_handles_labels()
        if handles:
            axis.legend(loc="upper right")


def plot_timing_axes(axes, queue):
    data = queue["plot"]
    axes[0].plot(data["t"], data["q_short_fi_ms"], linewidth=0.8, label="estimated short input interval")
    axes[0].plot(data["t"], data["q_avg_fi_ms"], linewidth=1.1, label="estimated smoothed input interval")
    axes[0].plot(data["t"], data["q_out_fi_ms"], linewidth=1.1, label="commanded output interval")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Estimated Input Timing and Commanded Output Timing")
    axes[1].plot(data["t"], data["q_jitter_ms"], linewidth=1.0, label="estimated mean input jitter")
    axes[1].plot(data["t"], data["q_tail_jitter_ms"], linewidth=1.0, label="estimated tail input jitter")
    axes[1].set_ylabel("ms")
    axes[1].set_title("Estimated Input Jitter")
    _style_axes(axes)


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
    _style_axes(axes)


def plot_latency_axes(axes, queue):
    data = queue["plot"]
    axes[0].plot(data["t"], data["estimated_current_delay_ms"], linewidth=1.0,
                 label="estimated current queue delay")
    axes[0].plot(data["t"], data["estimated_target_delay_ms"], linewidth=1.0,
                 label="estimated target queue delay")
    axes[0].plot(data["t"], data["estimated_raw_delay_ms"], linewidth=0.9,
                 label="estimated raw-controller queue delay")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Estimated Jitter-Buffer Delay (Not End-to-End Latency)")
    axes[1].plot(data["t"], data["q_buffered_frames"], linewidth=1.0, label="actual depth")
    axes[1].plot(data["t"], data["q_adaptive_depth"], linewidth=1.0, label="target depth")
    axes[1].plot(data["t"], data["q_depth_raw"], linewidth=0.9, label="raw controller depth")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Jitter-Buffer Depth")
    _style_axes(axes)


def plot_network_axes(axes, queue):
    data = queue["plot"]
    plotted_event = False
    for key, label in (("q_skip_delta", "inferred missing / skip"), ("q_drop_delta", "drop"),
                       ("q_dup_delta", "duplicate"), ("q_late_delta", "late"),
                       ("q_reorder_delta", "reorder"), ("q_stale_delta", "stale"),
                       ("q_ovf_delta", "overflow")):
        if any(math.isfinite(value) and value > 0.0 for value in data[key]):
            axes[0].plot(data["t"], data[key], linewidth=0.9, label=label)
            plotted_event = True
    if not plotted_event:
        axes[0].text(0.5, 0.5, "No queue events", transform=axes[0].transAxes,
                     ha="center", va="center", color="tab:green")
    jitter = [1.5 * value for value in data["q_tail_jitter_frames"]]
    axes[1].plot(data["t"], jitter, linewidth=1.1, label="tail jitter driver")
    if any(math.isfinite(value) and value > 0.0 for value in data["q_disorder_frames"]):
        axes[1].plot(data["t"], data["q_disorder_frames"], linewidth=0.9, label="reorder driver")
    if any(math.isfinite(value) and value > 0.0 for value in data["q_max_disorder_depth"]):
        axes[1].plot(data["t"], data["q_max_disorder_depth"], linewidth=0.8,
                     label="max observed disorder depth")
    pressure = [float("nan") if not math.isfinite(value) else (1.0 if value > 0.0 else 0.0)
                for value in data["q_pressure_frames"]]
    if any(math.isfinite(value) and value > 0.0 for value in pressure):
        axes[1].plot(data["t"], pressure, linewidth=0.8, label="pressure active")
    axes[0].set_title("ReliableUDP Queue Events")
    axes[0].set_ylabel("count / sample")
    axes[1].set_title("ReliableUDP Controller Drivers")
    axes[1].set_ylabel("frames")
    _style_axes(axes)


def write_core_report(queue, path):
    def write_stats(stream, label, values, unit):
        stream.write("{}: mean={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f} {}\n".format(
            label, values["mean"], values["p50"], values["p95"], values["p99"], values["max"], unit))

    with open(path, "w") as stream:
        stream.write("ReliableUDP queue statistics\n============================\n")
        stream.write("rows={}\nsessions={}\n\n".format(queue["count"], len(queue["sessions"])))
        for session in queue["sessions"]:
            stream.write("session_id={session_id} start_utc={start_utc} rows={rows} start_s={start_s:.6f} "
                         "end_s={end_s:.6f}\n".format(**session))
        stream.write("\nEstimated input timing\n")
        write_stats(stream, "short input interval", queue["short_input"], "ms")
        write_stats(stream, "smoothed input interval", queue["input"], "ms")
        write_stats(stream, "mean input jitter", queue["jitter"], "ms")
        write_stats(stream, "tail input jitter", queue["tail_jitter"], "ms")
        write_stats(stream, "tail input jitter", queue["tail_jitter_frames"], "frames")

        stream.write("\nCommanded output timing\n")
        write_stats(stream, "commanded output interval", queue["output"], "ms")
        stream.write("note: commanded output interval is controller state, not measured user output timing\n")

        stream.write("\nEstimated jitter-buffer delay\n")
        write_stats(stream, "current queue delay", queue["estimated_delay"]["current"], "ms")
        write_stats(stream, "target queue delay", queue["estimated_delay"]["target"], "ms")
        write_stats(stream, "raw-controller queue delay", queue["estimated_delay"]["raw"], "ms")
        stream.write("note: depth multiplied by commanded output interval; not end-to-end latency\n")

        stream.write("\nBuffer controller\n")
        write_stats(stream, "actual depth", queue["buffered"], "frames")
        write_stats(stream, "target depth", queue["target"], "frames")
        write_stats(stream, "raw controller depth", queue["raw_depth"], "frames")
        stream.write("pressure_active_pct={:.3f}\n".format(100.0 * queue["pressure_ratio"]))

        stream.write("\nReorder state\n")
        write_stats(stream, "effective disorder depth", queue["disorder"], "frames")
        write_stats(stream, "max observed disorder depth", queue["max_disorder"], "frames")

        counters = queue["counters"]
        stream.write("\nCounters\n")
        for key in ("recv", "dlv", "skip", "drop", "dup", "late", "reorder", "stale", "overflow"):
            stream.write("{}={}\n".format(key, int(counters[key])))
        delivery_ratio = counters["dlv"] / counters["recv"] if counters["recv"] > 0 else 0.0
        expected = counters["dlv"] + counters["skip"]
        inferred_missing_ratio = counters["skip"] / expected if expected > 0 else 0.0
        stream.write("delivery_to_received_ratio={:.6f}\n".format(delivery_ratio))
        stream.write("inferred_missing_sequence_ratio={:.6f}\n".format(inferred_missing_ratio))

        stream.write("\nIdle gaps\ncount={}\n".format(len(queue["gaps"])))
        if queue["gaps"]:
            stream.write("max_idle_gap_s={:.3f}\n".format(max(gap["idle_s"] for gap in queue["gaps"])))

        stream.write("\nUnavailable from product queue stats\n")
        stream.write("- measured end-to-end latency\n")
        stream.write("- measured per-frame user output interval\n")
        stream.write("- sender payload rate and sender interval distribution\n")
        stream.write("- true link loss rate; skip is only an inferred missing-sequence counter\n")


def remove_product_outputs(out_dir):
    for pattern in ("queue_timing_*.png", "queue_latency_*.png", "queue_network_*.png"):
        for path in glob.glob(os.path.join(out_dir, pattern)):
            os.remove(path)
    for name in ("queue_controller.png", "queue_network.png"):
        path = os.path.join(out_dir, name)
        if os.path.exists(path):
            os.remove(path)


def render_product_outputs(queue, out_dir, window_seconds):
    remove_product_outputs(out_dir)
    plotters = (("queue_timing", plot_timing_axes), ("queue_latency", plot_latency_axes),
                ("queue_network", plot_network_axes))
    for page, (start_s, end_s) in enumerate(window_ranges(queue, window_seconds), 1):
        window = dict(queue)
        window["plot"] = queue_plot_window(queue, start_s, end_s)
        suffix = "_{:03d}_{:07.0f}-{:07.0f}s.png".format(page, start_s, end_s)
        for name, plotter in plotters:
            fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
            plotter(axes, window)
            axes[-1].set_xlabel("time since first retained sample (s)")
            axes[-1].set_xlim(start_s, end_s)
            fig.tight_layout()
            fig.savefig(os.path.join(out_dir, name + suffix), dpi=200)
            plt.close(fig)
    write_core_report(queue, os.path.join(out_dir, "queue_report.txt"))


def main():
    parser = argparse.ArgumentParser(description="Analyze ReliableUDP product queue statistics")
    parser.add_argument("out_dir", help="directory containing queue_stats.csv and optional rotated files")
    parser.add_argument("--plot-window-seconds", type=float, default=600.0,
                        help="seconds per plot page (default: %(default)s)")
    args = parser.parse_args()
    if not math.isfinite(args.plot_window_seconds) or args.plot_window_seconds <= 0.0:
        parser.error("--plot-window-seconds must be greater than zero")
    queue = process_queue(os.path.join(args.out_dir, "queue_stats.csv"))
    render_product_outputs(queue, args.out_dir, args.plot_window_seconds)


if __name__ == "__main__":
    main()
