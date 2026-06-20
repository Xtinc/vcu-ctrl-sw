#!/usr/bin/env python3
import argparse
import csv
import math
import os

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.makedirs(os.environ["MPLCONFIGDIR"], exist_ok=True)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SAVE_DPI = 300
OUTPUTS = {
    "output_smoothness.png",
    "controller_terms.png",
    "network_effects.png",
    "controller_assessment.txt",
}
PLOT_OUTPUTS = OUTPUTS - {"controller_assessment.txt"}
OBSOLETE_OUTPUTS = {
    "arrival_intervals.png",
    "latency.png",
    "queue_timing.png",
    "queue_depth.png",
    "controller_tracking.png",
    "controller_assessment.png",
    "controller_dashboard.png",
    "queue_counters.png",
    "network_vs_controller.png",
    "latency_ideal_vs_actual.png",
    "parameter_influence.png",
}

ARRIVAL_COLUMNS = {
    "elapsed_ms",
    "seq",
    "interval_ms",
    "latency_ms",
    "size_bytes",
}
QUEUE_STATS_COLUMNS = {
    "elapsed_ms",
    "q_avg_fi_ms",
    "q_fb_fi_ms",
    "q_out_fi_ms",
    "q_jitter_ms",
    "q_tail_jitter_ms",
    "q_buffered_frames",
    "q_adaptive_depth",
    "q_depth_raw",
    "q_depth_error_frames",
    "q_skip_delta",
    "q_drop_delta",
    "q_late_delta",
    "q_reorder_delta",
    "q_stale_delta",
    "q_ovf_delta",
}
STAGE_COLUMNS = {
    "stage",
    "start_s",
    "end_s",
    "netem_args",
}


def fmt(value, digits=2):
    return ("{:.%df}" % digits).format(value)


def label_score(score):
    if score >= 90:
        return "GOOD"
    if score >= 70:
        return "WATCH"
    return "BAD"


def clamp(value, low, high):
    return max(low, min(value, high))


def read_csv(path, required_columns):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        fields = set(reader.fieldnames or ())
        missing = sorted(set(required_columns) - fields)
        if missing:
            raise SystemExit("{} is missing required columns: {}".format(path, ", ".join(missing)))
        return list(reader)


def read_summary(path):
    values = {}
    if not path or not os.path.exists(path):
        return values
    with open(path) as f:
        for line in f:
            key, separator, value = line.strip().partition("=")
            if separator:
                values[key] = value
    return values


def as_float(row, key, default=0.0):
    try:
        value = row.get(key, "")
        return default if value == "" else float(value)
    except ValueError:
        return default


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = p * (len(sorted_values) - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (pos - lo) * (sorted_values[hi] - sorted_values[lo])


def stats(values):
    ordered = sorted(values)
    return {
        "mean": sum(values) / float(len(values)) if values else 0.0,
        "p50": percentile(ordered, 0.50),
        "p90": percentile(ordered, 0.90),
        "p95": percentile(ordered, 0.95),
        "p99": percentile(ordered, 0.99),
        "max": max(values) if values else 0.0,
    }


def rolling_mean(values, window):
    out = []
    total = 0.0
    queue = []
    for value in values:
        queue.append(value)
        total += value
        if len(queue) > window:
            total -= queue.pop(0)
        out.append(total / float(len(queue)))
    return out


def rolling_percentile(values, window, p):
    out = []
    queue = []
    for value in values:
        queue.append(value)
        if len(queue) > window:
            queue.pop(0)
        out.append(percentile(sorted(queue), p))
    return out


def rolling_cv(values, window):
    out = []
    queue = []
    for value in values:
        queue.append(value)
        if len(queue) > window:
            queue.pop(0)
        mean = sum(queue) / float(len(queue))
        if mean <= 0.0:
            out.append(0.0)
            continue
        variance = sum((v - mean) * (v - mean) for v in queue) / float(len(queue))
        out.append(math.sqrt(variance) / mean)
    return out


def set_robust_upper_ylim(ax, values, p=0.99, margin=1.2):
    ordered = sorted(v for v in values if v >= 0.0)
    if not ordered:
        return
    upper = max(1.0, percentile(ordered, p) * margin)
    if max(ordered) > upper:
        ax.set_ylim(bottom=0.0, top=upper)
    else:
        ax.set_ylim(bottom=0.0)


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

    spec = {
        "delay_ms": 0.0,
        "jitter_ms": 0.0,
        "slot_min_ms": 0.0,
        "slot_max_ms": 0.0,
        "theoretical_min_latency_ms": 0.0,
        "theoretical_high_latency_ms": 0.0,
        "reorder_pct": 0.0,
        "loss_pct": 0.0,
    }
    tokens = args.split()
    for idx, token in enumerate(tokens):
        if token == "delay" and idx + 1 < len(tokens):
            spec["delay_ms"] = number(tokens[idx + 1])
            if idx + 2 < len(tokens):
                spec["jitter_ms"] = number(tokens[idx + 2])
        elif token == "slot" and idx + 1 < len(tokens):
            spec["slot_min_ms"] = number(tokens[idx + 1])
            spec["slot_max_ms"] = spec["slot_min_ms"]
            if idx + 2 < len(tokens) and tokens[idx + 2] not in ("packets", "bytes"):
                spec["slot_max_ms"] = number(tokens[idx + 2])
        elif token == "reorder" and idx + 1 < len(tokens):
            spec["reorder_pct"] = number(tokens[idx + 1])
        elif token == "loss" and idx + 1 < len(tokens):
            spec["loss_pct"] = number(tokens[idx + 1])
    spec["theoretical_min_latency_ms"] = max(0.0, spec["delay_ms"] - spec["jitter_ms"]) + spec["slot_min_ms"]
    spec["theoretical_high_latency_ms"] = spec["delay_ms"] + 2.0 * spec["jitter_ms"] + spec["slot_max_ms"]
    spec["ideal_latency_ms"] = spec["theoretical_high_latency_ms"]
    return spec


def read_stages(out_dir):
    path = os.path.join(out_dir, "tc_stages.csv")
    if not os.path.exists(path):
        return []
    stages = []
    for row in read_csv(path, STAGE_COLUMNS):
        start_s = as_float(row, "start_s")
        end_s = as_float(row, "end_s")
        if end_s > start_s:
            stage = {"stage": row.get("stage", ""), "start_s": start_s, "end_s": end_s}
            stage.update(parse_netem_args(row.get("netem_args", "")))
            stages.append(stage)
    return stages


def stage_step_series(stages, key):
    times = []
    values = []
    for stage in stages:
        times.extend([stage["start_s"], stage["end_s"]])
        values.extend([stage.get(key, 0.0), stage.get(key, 0.0)])
    return times, values


def stage_color(name):
    return {
        "normal": "tab:green",
        "jitter": "tab:orange",
        "jitter2": "tab:red",
        "reorder": "tab:red",
    }.get(name, "tab:gray")


def annotate_stages(axes, stages):
    if not stages:
        return
    axis_list = list(axes.flat) if hasattr(axes, "flat") else list(axes)
    for ax in axis_list:
        for stage in stages:
            ax.axvspan(stage["start_s"], stage["end_s"], color=stage_color(stage["stage"]), alpha=0.06, linewidth=0)
    for stage in stages:
        x = 0.5 * (stage["start_s"] + stage["end_s"])
        axis_list[0].text(
            x,
            0.98,
            stage["stage"],
            transform=axis_list[0].get_xaxis_transform(),
            ha="center",
            va="top",
            fontsize=9,
            color=stage_color(stage["stage"]),
        )


def save_figure(fig, out_dir, filename):
    if filename not in PLOT_OUTPUTS:
        raise ValueError("unexpected plot output: {}".format(filename))
    fig.tight_layout()
    path = os.path.join(out_dir, filename)
    fig.savefig(path, dpi=SAVE_DPI)
    plt.close(fig)
    print("wrote {}".format(path))


def remove_old_outputs(out_dir):
    for name in OBSOLETE_OUTPUTS | OUTPUTS:
        path = os.path.join(out_dir, name)
        if os.path.exists(path):
            os.remove(path)


def write_assessment(out_dir, metrics):
    path = os.path.join(out_dir, "controller_assessment.txt")
    with open(path, "w") as f:
        f.write("ReliableUDP output smoothness assessment\n")
        f.write("========================================\n\n")

        f.write("Overall\n")
        f.write("- score: {} / 100 ({})\n".format(int(round(metrics["score"])), label_score(metrics["score"])))
        f.write("- summary: {}\n".format(metrics["summary"]))
        f.write("- samples: stats={}, arrivals={}\n\n".format(metrics["stats_count"], metrics["arrival_count"]))

        if metrics.get("end_to_end"):
            accounting = metrics["end_to_end"]
            f.write("End-to-end message accounting\n")
            f.write("- sent: {}\n".format(accounting["sent"]))
            f.write("- received: {}\n".format(accounting["received"]))
            f.write("- send_fail: {}\n".format(accounting["send_fail"]))
            f.write("- observed_message_loss_pct: {}\n\n".format(fmt(accounting["loss_pct"], 3)))

        f.write("1. User-visible output interval smoothness\n")
        for key in ("mean", "p50", "p90", "p95", "p99", "max"):
            f.write("- interval_{}: {} ms\n".format(key, fmt(metrics["interval"].get(key, 0.0))))
        f.write("- rolling_cv_mean: {}\n".format(fmt(metrics["cv"].get("mean", 0.0), 4)))
        f.write("- rolling_cv_p95: {}\n".format(fmt(metrics["cv"].get("p95", 0.0), 4)))
        f.write("- see output_smoothness.png\n\n")

        f.write("2. User-visible latency\n")
        for key in ("mean", "p50", "p90", "p95", "p99", "max"):
            f.write("- latency_{}: {} ms\n".format(key, fmt(metrics["latency"].get(key, 0.0))))
        f.write("- ideal_latency_max: {} ms\n".format(fmt(metrics["ideal_latency_max"])))
        f.write("\n")

        f.write("3. Controller output interval terms\n")
        f.write("- feedforward_interval_p95: {} ms\n".format(fmt(metrics["controller"]["feedforward_p95"])))
        f.write("- feedback_interval_p05: {} ms\n".format(fmt(metrics["controller"]["feedback_p05"])))
        f.write("- feedback_interval_p95: {} ms\n".format(fmt(metrics["controller"]["feedback_p95"])))
        f.write("- output_interval_p95: {} ms\n".format(fmt(metrics["controller"]["output_p95"])))
        f.write("- see controller_terms.png\n\n")

        f.write("4. Buffer depth and visible failures\n")
        f.write("- buffered_p95: {} frames\n".format(fmt(metrics["depth"]["buffered_p95"])))
        f.write("- target_p95: {} frames\n".format(fmt(metrics["depth"]["target_p95"])))
        f.write("- depth_error_p95: {} frames\n".format(fmt(metrics["depth"]["error_p95"])))
        for key in ("skip", "late", "overflow", "drop", "stale", "reorder"):
            f.write("- {}: {}\n".format(key, int(metrics["events"].get(key, 0.0))))
        f.write("\n")

        if metrics["stage_effects"]:
            f.write("5. Controller target drivers\n")
            f.write("- see network_effects.png\n")
            f.write(
                "stage,start_s,end_s,target_p50_frames,target_p95_frames,raw_p50_frames,"
                "reorder_p95_frames,jitter_driver_p95_frames,pressure_p95_frames,"
                "latency_p50_ms,latency_p95_ms,reorder_events,bad_events\n"
            )
            for row in metrics["stage_effects"]:
                f.write(
                    "{stage},{start_s:.0f},{end_s:.0f},{target_p50_frames:.2f},{target_p95_frames:.2f},"
                    "{raw_p50_frames:.2f},{reorder_p95_frames:.2f},{jitter_driver_p95_frames:.2f},"
                    "{pressure_p95_frames:.2f},"
                    "{latency_p50_ms:.2f},{latency_p95_ms:.2f},{reorder_events:.0f},{bad_events:.0f}\n".format(**row)
                )
    print("wrote {}".format(path))


def main():
    parser = argparse.ArgumentParser(
        description="Generate ReliableUDP jitter/controller plots from a capture directory."
    )
    parser.add_argument("out_dir", help="directory containing arrival_intervals.csv and queue_stats.csv")
    parser.add_argument("--sender-summary", help="optional sender_summary.txt for end-to-end loss accounting")
    args = parser.parse_args()

    out_dir = args.out_dir
    sender_summary = read_summary(args.sender_summary)
    arrival_path = os.path.join(out_dir, "arrival_intervals.csv")
    stats_path = os.path.join(out_dir, "queue_stats.csv")
    if not os.path.exists(arrival_path):
        raise SystemExit("missing input file: {}".format(arrival_path))
    if not os.path.exists(stats_path):
        raise SystemExit("missing input file: {}".format(stats_path))

    arrivals = read_csv(arrival_path, ARRIVAL_COLUMNS)
    queue_stats = read_csv(stats_path, QUEUE_STATS_COLUMNS)
    if not arrivals:
        raise SystemExit("arrival_intervals.csv has no data rows")
    if not queue_stats:
        raise SystemExit("queue_stats.csv has no data rows")

    stages = read_stages(out_dir)
    t_arr = [as_float(r, "elapsed_ms") / 1000.0 for r in arrivals]
    output_intervals = [as_float(r, "interval_ms") for r in arrivals if as_float(r, "interval_ms") > 0.0]
    t_out = [t for t, r in zip(t_arr, arrivals) if as_float(r, "interval_ms") > 0.0]
    latency_samples = [
        (t, as_float(r, "latency_ms"))
        for t, r in zip(t_arr, arrivals)
        if math.isfinite(as_float(r, "latency_ms")) and as_float(r, "latency_ms") >= 0.0
    ]
    t_latency = [sample[0] for sample in latency_samples]
    latencies = [sample[1] for sample in latency_samples]

    t_stats = [as_float(r, "elapsed_ms") / 1000.0 for r in queue_stats]
    q_avg_fi = [as_float(r, "q_avg_fi_ms") for r in queue_stats]
    q_fb_fi = [as_float(r, "q_fb_fi_ms") for r in queue_stats]
    q_out_fi = [as_float(r, "q_out_fi_ms") for r in queue_stats]
    q_jitter = [as_float(r, "q_jitter_ms") for r in queue_stats]
    q_tail_jitter = [as_float(r, "q_tail_jitter_ms") for r in queue_stats]
    q_input_jitter_hi = [avg + jitter for avg, jitter in zip(q_avg_fi, q_jitter)]
    q_input_tail_hi = [avg + tail for avg, tail in zip(q_avg_fi, q_tail_jitter)]
    q_buffered = [as_float(r, "q_buffered_frames") for r in queue_stats]
    q_target = [as_float(r, "q_adaptive_depth") for r in queue_stats]
    q_depth_raw = [as_float(r, "q_depth_raw") for r in queue_stats]
    q_depth_error = [as_float(r, "q_depth_error_frames") for r in queue_stats]
    q_skip = [as_float(r, "q_skip_delta") for r in queue_stats]
    q_drop = [as_float(r, "q_drop_delta") for r in queue_stats]
    q_late = [as_float(r, "q_late_delta") for r in queue_stats]
    q_reorder = [as_float(r, "q_reorder_delta") for r in queue_stats]
    q_stale = [as_float(r, "q_stale_delta") for r in queue_stats]
    q_ovf = [as_float(r, "q_ovf_delta") for r in queue_stats]
    q_bad = [skip + late + ovf for skip, late, ovf in zip(q_skip, q_late, q_ovf)]
    q_reorder_driver = [as_float(r, "q_disorder_frames") for r in queue_stats]
    q_jitter_driver = [1.5 * as_float(r, "q_tail_jitter_frames") for r in queue_stats]
    q_pressure_driver = [1.0 if as_float(r, "q_pressure_frames") > 0.0 else 0.0 for r in queue_stats]
    q_margin_driver = [0.5 for _ in queue_stats]

    window = max(5, min(100, len(output_intervals) // 20 if len(output_intervals) >= 20 else 5))
    interval_mean = rolling_mean(output_intervals, window)
    interval_p95 = rolling_percentile(output_intervals, window, 0.95)
    interval_cv = rolling_cv(output_intervals, window)
    latency_mean = rolling_mean(latencies, window)
    latency_p95 = rolling_percentile(latencies, window, 0.95)
    reorder_t, reorder_pct = stage_step_series(stages, "reorder_pct")
    loss_t, loss_pct = stage_step_series(stages, "loss_pct")
    ideal_t, ideal_latency = stage_step_series(stages, "ideal_latency_ms")
    stage_effects = []
    for stage in stages:
        start_s = stage["start_s"]
        end_s = stage["end_s"]
        latency_values = sorted(v for t, v in zip(t_latency, latencies) if start_s <= t < end_s)
        target_values = sorted(v for t, v in zip(t_stats, q_target) if start_s <= t < end_s)
        raw_values = sorted(v for t, v in zip(t_stats, q_depth_raw) if start_s <= t < end_s)
        reorder_driver_values = sorted(v for t, v in zip(t_stats, q_reorder_driver) if start_s <= t < end_s)
        jitter_driver_values = sorted(v for t, v in zip(t_stats, q_jitter_driver) if start_s <= t < end_s)
        pressure_driver_values = sorted(v for t, v in zip(t_stats, q_pressure_driver) if start_s <= t < end_s)
        bad_events = sum(v for t, v in zip(t_stats, q_bad) if start_s <= t < end_s)
        reorder_events = sum(v for t, v in zip(t_stats, q_reorder) if start_s <= t < end_s)
        stage_effects.append(
            {
                "stage": stage["stage"],
                "start_s": start_s,
                "end_s": end_s,
                "target_p50_frames": percentile(target_values, 0.50),
                "target_p95_frames": percentile(target_values, 0.95),
                "raw_p50_frames": percentile(raw_values, 0.50),
                "reorder_p95_frames": percentile(reorder_driver_values, 0.95),
                "jitter_driver_p95_frames": percentile(jitter_driver_values, 0.95),
                "pressure_p95_frames": percentile(pressure_driver_values, 0.95),
                "latency_p50_ms": percentile(latency_values, 0.50),
                "latency_p95_ms": percentile(latency_values, 0.95),
                "reorder_events": reorder_events,
                "bad_events": bad_events,
            }
        )

    remove_old_outputs(out_dir)

    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=False)
    axes[0].plot(t_out, output_intervals, linewidth=0.5, alpha=0.28, label="output interval samples")
    axes[0].plot(t_out, interval_mean, linewidth=1.2, label="rolling mean")
    axes[0].plot(t_out, interval_p95, linewidth=1.2, label="rolling p95")
    axes[0].set_ylabel("ms")
    axes[0].set_title("User-Visible Output Interval Smoothness")
    set_robust_upper_ylim(axes[0], output_intervals)
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].plot(t_latency, latency_mean, linewidth=1.1, label="latency rolling mean")
    axes[1].plot(t_latency, latency_p95, linewidth=1.1, label="latency rolling p95")
    if ideal_t:
        axes[1].plot(
            ideal_t,
            ideal_latency,
            linewidth=1.1,
            linestyle="--",
            label="configured high latency envelope",
        )
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("ms")
    axes[1].set_title("User-Visible Latency")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "output_smoothness.png")

    fig, axes = plt.subplots(3, 1, figsize=(18, 12), sharex=True)
    axes[0].plot(t_stats, q_avg_fi, linewidth=1.1, label="feed-forward interval")
    axes[0].plot(t_stats, q_out_fi, linewidth=1.1, label="final output interval")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Controller Output Interval")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].plot(t_stats, q_avg_fi, linewidth=1.0, label="mean arrival interval")
    axes[1].fill_between(t_stats, q_avg_fi, q_input_tail_hi, alpha=0.16, label="tail arrival interval envelope")
    axes[1].plot(t_stats, q_input_jitter_hi, linewidth=0.9, label="mean + input jitter")
    axes[1].plot(t_stats, q_input_tail_hi, linewidth=0.9, label="mean + tail jitter")
    axes[1].plot(t_stats, q_out_fi, linewidth=0.9, alpha=0.75, label="final output interval")
    axes[1].set_ylabel("ms")
    axes[1].set_title("Estimated Input Arrival Interval Envelope")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")

    axes[2].plot(t_stats, q_buffered, linewidth=1.0, label="actual buffered depth")
    axes[2].plot(t_stats, q_target, linewidth=1.0, label="target buffered depth")
    axes[2].plot(t_stats, q_depth_error, linewidth=0.9, label="depth error")
    axes[2].plot(t_stats, q_bad, linewidth=0.9, label="skip + late + overflow")
    axes[2].axhline(0.0, color="black", linewidth=1.0, alpha=0.35)
    axes[2].set_xlabel("time (s)")
    axes[2].set_ylabel("frames / count")
    axes[2].set_title("Buffer Depth and Visible Failures")
    axes[2].grid(True, alpha=0.25)
    axes[2].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "controller_terms.png")

    fig, axes = plt.subplots(2, 1, figsize=(18, 9), sharex=True)
    axes[0].plot(t_stats, q_reorder, linewidth=1.0, label="observed reorder/disorder events")
    axes[0].plot(t_stats, q_stale, linewidth=0.9, label="stale drops")
    axes[0].plot(t_stats, q_drop, linewidth=0.9, label="drops")
    if reorder_t and max(reorder_pct + loss_pct) > 0.0:
        ax_param = axes[0].twinx()
        ax_param.plot(reorder_t, reorder_pct, linewidth=0.9, linestyle="--", color="tab:purple", label="configured reorder")
        ax_param.plot(loss_t, loss_pct, linewidth=0.9, linestyle="--", color="tab:brown", label="configured loss")
        ax_param.set_ylabel("%")
        lines, labels = axes[0].get_legend_handles_labels()
        param_lines, param_labels = ax_param.get_legend_handles_labels()
        axes[0].legend(lines + param_lines, labels + param_labels, loc="upper right")
    else:
        axes[0].legend(loc="upper right")
    axes[0].set_ylabel("count / stats period")
    axes[0].set_title("Observed Network Disorder")
    axes[0].grid(True, alpha=0.25)

    axes[1].plot(t_stats, q_reorder_driver, linewidth=1.0, label="reorder driver")
    axes[1].plot(t_stats, q_jitter_driver, linewidth=1.0, label="tail jitter driver = 1.5 * tail jitter frames")
    axes[1].plot(t_stats, q_pressure_driver, linewidth=0.9, label="pressure driver")
    axes[1].plot(t_stats, q_margin_driver, linewidth=0.8, linestyle=":", label="margin")
    axes[1].plot(t_stats, q_depth_raw, linewidth=1.0, linestyle="--", label="raw target sum")
    axes[1].legend(loc="upper right")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Controller Target Drivers")
    axes[1].grid(True, alpha=0.25)
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "network_effects.png")

    interval_stats = stats(output_intervals)
    latency_stats = stats(latencies)
    cv_stats = stats(interval_cv)
    event_totals = {
        "skip": sum(q_skip),
        "late": sum(q_late),
        "overflow": sum(q_ovf),
        "drop": sum(q_drop),
        "stale": sum(q_stale),
        "reorder": sum(q_reorder),
    }
    visible_failures = event_totals["skip"] + event_totals["late"] + event_totals["overflow"]
    score = 100.0
    score -= min(45.0, cv_stats["p95"] * 50.0)
    score -= min(30.0, visible_failures * 10.0)
    score -= min(15.0, max(0.0, interval_stats["p99"] - interval_stats["p50"]) * 0.5)
    score = clamp(score, 0.0, 100.0)
    summary = "stable output intervals" if visible_failures == 0 and cv_stats["p95"] < 0.5 else "output smoothness needs attention"
    end_to_end = None
    if sender_summary:
        sent = int(sender_summary.get("sent_count", "0"))
        send_fail = int(sender_summary.get("send_fail_count", "0"))
        received = len(arrivals)
        loss_pct = 100.0 * max(0, sent - received) / float(sent) if sent else 0.0
        end_to_end = {"sent": sent, "received": received, "send_fail": send_fail, "loss_pct": loss_pct}
    write_assessment(
        out_dir,
        {
            "score": score,
            "summary": summary,
            "stats_count": len(queue_stats),
            "arrival_count": len(arrivals),
            "interval": interval_stats,
            "cv": cv_stats,
            "latency": latency_stats,
            "ideal_latency_max": max(ideal_latency) if ideal_latency else 0.0,
            "controller": {
                "feedforward_p95": stats(q_avg_fi)["p95"],
                "feedback_p05": percentile(sorted(q_fb_fi), 0.05) if q_fb_fi else 0.0,
                "feedback_p95": stats(q_fb_fi)["p95"],
                "output_p95": stats(q_out_fi)["p95"],
            },
            "depth": {
                "buffered_p95": stats(q_buffered)["p95"],
                "target_p95": stats(q_target)["p95"],
                "error_p95": stats(q_depth_error)["p95"],
            },
            "events": event_totals,
            "stage_effects": stage_effects,
            "end_to_end": end_to_end,
        },
    )


if __name__ == "__main__":
    main()
