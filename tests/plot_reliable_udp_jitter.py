#!/usr/bin/env python3
import csv
import math
import os
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
os.makedirs(os.environ["MPLCONFIGDIR"], exist_ok=True)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

FIGSIZE = (18, 7)
SAVE_DPI = 300
SCATTER_SIZE = 8
SCATTER_ALPHA = 0.65
JITTER_WEIGHT = 2.0
DEPTH_MARGIN_FRAMES = 0.5
IMPORTANT_OUTPUTS = {
    "network_vs_controller.png",
    "latency_ideal_vs_actual.png",
    "parameter_influence.png",
    "controller_assessment.txt",
}
PLOT_OUTPUTS = IMPORTANT_OUTPUTS - {"controller_assessment.txt"}
OBSOLETE_OUTPUTS = {
    "arrival_intervals.png",
    "latency.png",
    "queue_timing.png",
    "queue_depth.png",
    "controller_tracking.png",
    "controller_assessment.png",
    "controller_dashboard.png",
    "queue_counters.png",
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
    "q_jitter_ms",
    "q_jitter_frames",
    "q_disorder_frames",
    "q_max_disorder_depth",
    "q_tail_jitter_ms",
    "q_tail_jitter_frames",
    "q_buffered_frames",
    "q_adaptive_depth",
    "q_depth_raw",
    "q_depth_error_frames",
    "q_pressure_frames",
    "q_pacing_factor",
    "q_recv_delta",
    "q_dlv_delta",
    "q_skip_delta",
    "q_drop_delta",
    "q_dup_delta",
    "q_late_delta",
    "q_reorder_delta",
    "q_stale_delta",
    "q_ovf_delta",
    "recv_rate_bps",
    "lost_rate",
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


def read_csv(path, required_columns=None):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        required = set(required_columns or ())
        fields = set(reader.fieldnames or ())
        missing = sorted(required - fields)
        if missing:
            raise SystemExit("{} is missing required columns: {}".format(path, ", ".join(missing)))
        return list(reader)


def as_float(row, key, default=0.0):
    value = row.get(key, "")
    if value == "":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def rolling_mean(values, window):
    if not values:
        return []
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


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = p * (len(sorted_values) - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (pos - lo) * (sorted_values[hi] - sorted_values[lo])


def percentile_values(values):
    s = sorted(values)
    return {
        "p50": percentile(s, 0.50),
        "p90": percentile(s, 0.90),
        "p95": percentile(s, 0.95),
        "p99": percentile(s, 0.99),
        "max": max(s) if s else 0.0,
        "mean": sum(s) / float(len(s)) if s else 0.0,
    }


def total(values):
    return sum(values) if values else 0.0


def mean(values):
    return sum(values) / float(len(values)) if values else 0.0


def top_rows(times, values, count=5):
    pairs = sorted(zip(values, times), reverse=True)
    return [(t, v) for v, t in pairs[:count] if v > 0.0]


def rolling_percentile(values, window, p):
    out = []
    queue = []
    for value in values:
        queue.append(value)
        if len(queue) > window:
            queue.pop(0)
        out.append(percentile(sorted(queue), p))
    return out


def require_file(path):
    if not os.path.exists(path):
        raise SystemExit("missing input file: {}".format(path))


def remove_legacy_outputs(out_dir):
    for name in OBSOLETE_OUTPUTS:
        path = os.path.join(out_dir, name)
        if os.path.exists(path):
            os.remove(path)


def save_figure(fig, out_dir, filename):
    if filename not in PLOT_OUTPUTS:
        raise ValueError("unexpected plot output: {}".format(filename))
    fig.tight_layout()
    output_path = os.path.join(out_dir, filename)
    fig.savefig(output_path, dpi=SAVE_DPI)
    plt.close(fig)
    print("wrote {}".format(output_path))


def read_stages(out_dir):
    path = os.path.join(out_dir, "tc_stages.csv")
    if not os.path.exists(path):
        return []
    stages = []
    for row in read_csv(path, STAGE_COLUMNS):
        try:
            start_s = float(row.get("start_s", "0"))
            end_s = float(row.get("end_s", "0"))
        except ValueError:
            raise SystemExit("{} has invalid stage timing: {}".format(path, row))
        if end_s <= start_s:
            raise SystemExit("{} has non-positive stage duration: {}".format(path, row))
        stages.append(
            {
                "stage": row.get("stage", ""),
                "start_s": start_s,
                "end_s": end_s,
                "netem_args": row.get("netem_args", ""),
            }
        )
    return stages


def stage_color(stage):
    return {
        "normal": "tab:green",
        "jitter": "tab:orange",
        "jitter2": "tab:red",
    }.get(stage, "tab:gray")


def annotate_stages(axes, stages):
    if not stages:
        return
    if hasattr(axes, "flat"):
        axis_list = list(axes.flat)
    elif isinstance(axes, (list, tuple)):
        axis_list = list(axes)
    else:
        axis_list = [axes]

    for ax in axis_list:
        for stage in stages:
            ax.axvspan(stage["start_s"], stage["end_s"], color=stage_color(stage["stage"]), alpha=0.06, linewidth=0)

    top = axis_list[0]
    for stage in stages:
        x = 0.5 * (stage["start_s"] + stage["end_s"])
        top.text(
            x,
            0.98,
            stage["stage"],
            transform=top.get_xaxis_transform(),
            ha="center",
            va="top",
            fontsize=9,
            color=stage_color(stage["stage"]),
        )


def parse_percent(value):
    try:
        return float(value.rstrip("%"))
    except (AttributeError, ValueError):
        return 0.0


def parse_time_ms(value):
    try:
        if value.endswith("ms"):
            return float(value[:-2])
        if value.endswith("s"):
            return float(value[:-1]) * 1000.0
        return float(value)
    except (AttributeError, ValueError):
        return 0.0


def parse_netem_args(args):
    tokens = args.split()
    out = {
        "delay_ms": 0.0,
        "jitter_ms": 0.0,
        "reorder_pct": 0.0,
        "loss_pct": 0.0,
    }
    for idx, token in enumerate(tokens):
        if token == "delay" and idx + 1 < len(tokens):
            out["delay_ms"] = parse_time_ms(tokens[idx + 1])
            if idx + 2 < len(tokens):
                out["jitter_ms"] = parse_time_ms(tokens[idx + 2])
        elif token == "reorder" and idx + 1 < len(tokens):
            out["reorder_pct"] = parse_percent(tokens[idx + 1])
        elif token == "loss" and idx + 1 < len(tokens):
            out["loss_pct"] = parse_percent(tokens[idx + 1])
    return out


def stage_step_series(stages, key):
    times = []
    values = []
    for stage in stages:
        spec = parse_netem_args(stage.get("netem_args", ""))
        value = spec[key]
        times.extend([stage["start_s"], stage["end_s"]])
        values.extend([value, value])
    return times, values


def ideal_latency_budget_series(stages):
    times = []
    values = []
    for stage in stages:
        spec = parse_netem_args(stage.get("netem_args", ""))
        value = spec["delay_ms"] + 2.0 * spec["jitter_ms"]
        times.extend([stage["start_s"], stage["end_s"]])
        values.extend([value, value])
    return times, values


def values_in_window(times, values, start_s, end_s):
    return [v for t, v in zip(times, values) if start_s <= t < end_s]


def stage_effect_rows(stages, t_stats, t_arr, metrics):
    rows = []
    for stage in stages:
        start = stage["start_s"]
        end = stage["end_s"]
        tail_start = start + 0.75 * max(0.0, end - start)
        spec = parse_netem_args(stage.get("netem_args", ""))

        def stat_tail(key, default=0.0):
            vals = values_in_window(t_stats, metrics[key], tail_start, end)
            return mean(vals) if vals else default

        def total_stage(key):
            return total(values_in_window(t_stats, metrics[key], start, end))

        latency_tail = values_in_window(t_arr, metrics["latency_ms"], tail_start, end)
        rows.append(
            {
                "stage": stage["stage"],
                "start_s": start,
                "end_s": end,
                "delay_ms": spec["delay_ms"],
                "jitter_ms": spec["jitter_ms"],
                "reorder_pct": spec["reorder_pct"],
                "loss_pct": spec["loss_pct"],
                "ideal_latency_ms": spec["delay_ms"] + 2.0 * spec["jitter_ms"],
                "tail_jitter_ms": stat_tail("q_tail_jitter_ms"),
                "adaptive_depth": stat_tail("q_adaptive_depth"),
                "buffered_frames": stat_tail("q_buffered_frames"),
                "pacing_factor": stat_tail("q_pacing_factor"),
                "latency_p95": percentile(sorted(latency_tail), 0.95) if latency_tail else 0.0,
                "bad_events": total_stage("q_bad"),
            }
        )
    return rows


def write_assessment(out_dir, metrics):
    output_path = os.path.join(out_dir, "controller_assessment.txt")
    with open(output_path, "w") as f:
        f.write("ReliableUDP jitter controller assessment\n")
        f.write("========================================\n\n")

        f.write("Overall\n")
        f.write("- score: {} / 100 ({})\n".format(int(round(metrics["score"])), label_score(metrics["score"])))
        f.write("- summary: {}\n".format(metrics["summary"]))
        f.write("- samples: stats={}, arrivals={}\n".format(metrics["sample_count"], metrics["arrival_count"]))
        f.write("\n")

        f.write("Key verdicts\n")
        for line in metrics["verdict"]:
            f.write("- {}\n".format(line))
        f.write("\n")

        f.write("1. Stability: does it avoid visible failure?\n")
        for key in ("skip", "late", "overflow", "drop", "stale", "reorder"):
            f.write("- {}: {}\n".format(key, int(metrics["counters"].get(key, 0))))
        f.write("- bad_event_rate_per_sample: {}\n".format(fmt(metrics["stability"]["bad_event_rate"], 4)))
        if metrics["events"]:
            f.write("- worst event samples:\n")
            for item in metrics["events"]:
                f.write("  t={}s, count={}\n".format(fmt(item[0]), fmt(item[1], 0)))
        else:
            f.write("- worst event samples: none\n")
        f.write("\n")

        f.write("2. Responsiveness: does depth track estimated need?\n")
        f.write("- expected_raw ~= max(disorder, max_disorder) + {} * tail_jitter_frames + {}\n".format(
            JITTER_WEIGHT, DEPTH_MARGIN_FRAMES
        ))
        for key in ("raw_mean", "raw_p95", "raw_max", "adaptive_mean", "adaptive_p95", "adaptive_max",
                    "depth_overhead_mean", "depth_overhead_p95"):
            f.write("- {}: {}\n".format(key, fmt(metrics["depth"].get(key, 0.0))))
        f.write("- response_ratio_p95: {}\n".format(fmt(metrics["responsiveness"]["response_ratio_p95"])))
        f.write("- tracking_error_abs_p95: {} frames\n".format(fmt(metrics["responsiveness"]["tracking_error_abs_p95"])))
        f.write("- pacing_factor_mean: {}\n".format(fmt(metrics["responsiveness"]["pacing_factor_mean"])))
        f.write("- pacing_factor_p95: {}\n".format(fmt(metrics["responsiveness"]["pacing_factor_p95"])))
        if metrics["depth_peaks"]:
            f.write("- highest adaptive-depth samples:\n")
            for item in metrics["depth_peaks"]:
                f.write("  t={}s, depth={}\n".format(fmt(item[0]), fmt(item[1], 0)))
        f.write("\n")

        f.write("3. Cost: how much latency/buffering is paid?\n")
        f.write("Latency ms\n")
        for key in ("mean", "p50", "p90", "p95", "p99", "max"):
            f.write("- {}: {}\n".format(key, fmt(metrics["latency"].get(key, 0.0))))
        f.write("Buffer/headroom frames\n")
        for key in ("headroom_mean", "headroom_p05", "headroom_min", "buffered_mean", "buffered_p95",
                    "buffered_max"):
            f.write("- {}: {}\n".format(key, fmt(metrics["depth"].get(key, 0.0))))
        f.write("\n")

        f.write("4. Explainability: can raw depth be explained by measured jitter/reorder?\n")
        for key in ("tail_jitter_frames_p95", "disorder_frames_p95", "raw_residual_mean", "raw_residual_p95",
                    "raw_residual_max", "pressure_component_p95"):
            f.write("- {}: {}\n".format(key, fmt(metrics["explain"].get(key, 0.0))))
        if metrics["residual_peaks"]:
            f.write("- highest unexplained-raw samples:\n")
            for item in metrics["residual_peaks"]:
                f.write("  t={}s, residual={}\n".format(fmt(item[0]), fmt(item[1])))
        f.write("\n")

        f.write("5. Parameter influence on raw target\n")
        f.write("- reorder_component_mean: {} frames\n".format(fmt(metrics["influence"]["reorder_mean"])))
        f.write("- jitter_component_mean: {} frames\n".format(fmt(metrics["influence"]["jitter_mean"])))
        f.write("- jitter_component_p95: {} frames\n".format(fmt(metrics["influence"]["jitter_p95"])))
        f.write("- margin_frames: {}\n".format(fmt(DEPTH_MARGIN_FRAMES)))
        f.write("- see parameter_influence.png for jitter_weight sensitivity\n")
        if metrics["stage_effects"]:
            f.write("\n")
            f.write("6. Preset network vs final controller effect\n")
            f.write(
                "stage,start_s,end_s,preset_delay_ms,preset_jitter_ms,preset_reorder_pct,"
                "ideal_latency_ms,actual_latency_p95_ms,tail_jitter_ms,adaptive_depth,"
                "buffered_frames,pacing_factor,bad_events\n"
            )
            for row in metrics["stage_effects"]:
                f.write(
                    "{stage},{start_s:.0f},{end_s:.0f},{delay_ms:.2f},{jitter_ms:.2f},{reorder_pct:.2f},"
                    "{ideal_latency_ms:.2f},{latency_p95:.2f},{tail_jitter_ms:.2f},"
                    "{adaptive_depth:.2f},{buffered_frames:.2f},{pacing_factor:.2f},{bad_events:.0f}\n".format(**row)
                )
    print("wrote {}".format(output_path))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 tests/plot_reliable_udp_jitter.py <out-dir>")

    out_dir = sys.argv[1]
    arrival_path = os.path.join(out_dir, "arrival_intervals.csv")
    stats_path = os.path.join(out_dir, "queue_stats.csv")
    require_file(arrival_path)
    require_file(stats_path)

    arrivals = read_csv(arrival_path, ARRIVAL_COLUMNS)
    stats = read_csv(stats_path, QUEUE_STATS_COLUMNS)
    stages = read_stages(out_dir)
    if not stages:
        raise SystemExit("tc_stages.csv has no data rows")
    if not arrivals:
        raise SystemExit("arrival_intervals.csv has no data rows")
    if not stats:
        raise SystemExit("queue_stats.csv has no data rows")

    t_arr = [as_float(r, "elapsed_ms") / 1000.0 for r in arrivals]
    intervals = [as_float(r, "interval_ms") for r in arrivals]
    latencies = [as_float(r, "latency_ms") for r in arrivals]
    window = max(5, min(100, len(intervals) // 20 if len(intervals) >= 20 else 5))
    interval_mean = rolling_mean(intervals, window)
    interval_p95 = rolling_percentile(intervals, window, 0.95)

    t_stats = [as_float(r, "elapsed_ms") / 1000.0 for r in stats]
    q_avg_fi = [as_float(r, "q_avg_fi_ms") for r in stats]
    q_jitter = [as_float(r, "q_jitter_ms") for r in stats]
    q_tail_jitter = [as_float(r, "q_tail_jitter_ms") for r in stats]
    q_tail_jitter_frames = [as_float(r, "q_tail_jitter_frames") for r in stats]
    q_disorder = [as_float(r, "q_disorder_frames") for r in stats]
    q_max_disorder = [as_float(r, "q_max_disorder_depth") for r in stats]
    q_buffered = [as_float(r, "q_buffered_frames") for r in stats]
    q_adaptive = [as_float(r, "q_adaptive_depth") for r in stats]
    q_raw = [as_float(r, "q_depth_raw") for r in stats]
    q_depth_error = [as_float(r, "q_depth_error_frames") for r in stats]
    q_pressure = [as_float(r, "q_pressure_frames") for r in stats]
    q_pace = [as_float(r, "q_pacing_factor") for r in stats]

    q_drop = [as_float(r, "q_drop_delta") for r in stats]
    q_late = [as_float(r, "q_late_delta") for r in stats]
    q_reorder = [as_float(r, "q_reorder_delta") for r in stats]
    q_skip = [as_float(r, "q_skip_delta") for r in stats]
    q_ovf = [as_float(r, "q_ovf_delta") for r in stats]
    q_stale = [as_float(r, "q_stale_delta") for r in stats]

    q_reorder_for_explain = [max(d, m) for d, m in zip(q_disorder, q_max_disorder)]
    q_jitter_component = [JITTER_WEIGHT * j for j in q_tail_jitter_frames]
    q_pressure_component = [
        max(0.0, raw - disorder - jitter - DEPTH_MARGIN_FRAMES)
        for raw, disorder, jitter in zip(q_raw, q_reorder_for_explain, q_jitter_component)
    ]
    q_network_need = [d + j for d, j in zip(q_reorder_for_explain, q_jitter_component)]
    q_expected_raw = [
        d + JITTER_WEIGHT * j + DEPTH_MARGIN_FRAMES for d, j in zip(q_reorder_for_explain, q_tail_jitter_frames)
    ]
    q_raw_residual = [max(0.0, raw - expected) for raw, expected in zip(q_raw, q_expected_raw)]
    q_headroom = [adaptive - buffered for adaptive, buffered in zip(q_adaptive, q_buffered)]
    q_depth_overhead = [max(0.0, adaptive - raw) for adaptive, raw in zip(q_adaptive, q_raw)]
    q_tracking_error = [adaptive - math.ceil(raw) for adaptive, raw in zip(q_adaptive, q_raw)]
    q_response_ratio = [
        adaptive / max(1.0, math.ceil(raw)) if raw > 0.0 else adaptive for adaptive, raw in zip(q_adaptive, q_raw)
    ]
    q_bad = [skip + late + ovf for skip, late, ovf in zip(q_skip, q_late, q_ovf)]

    remove_legacy_outputs(out_dir)

    latency_mean = rolling_mean(latencies, window)
    latency_p95 = rolling_percentile(latencies, window, 0.95)
    ideal_t, ideal_latency = ideal_latency_budget_series(stages)

    fig, axes = plt.subplots(4, 1, figsize=(18, 14), sharex=True)
    if stages:
        delay_t, delay_v = stage_step_series(stages, "delay_ms")
        jitter_t, jitter_v = stage_step_series(stages, "jitter_ms")
        reorder_t, reorder_v = stage_step_series(stages, "reorder_pct")
        axes[0].plot(delay_t, delay_v, linewidth=1.3, label="preset delay")
        axes[0].plot(jitter_t, jitter_v, linewidth=1.3, label="preset jitter")
        axes[1].plot(reorder_t, reorder_v, linewidth=1.3, label="preset reorder %")
    axes[0].plot(t_stats, q_tail_jitter, linewidth=1.1, label="estimated tail jitter")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Preset Network Volatility vs Controller Estimates")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].plot(t_stats, q_reorder, linewidth=1.0, label="observed reorder delta")
    axes[1].plot(t_stats, q_bad, linewidth=1.0, label="skip + late + overflow")
    axes[1].set_ylabel("% / count")
    axes[1].set_title("Preset Reorder vs Visible Failures")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")

    axes[2].plot(t_stats, q_raw, linewidth=1.1, label="raw target")
    axes[2].plot(t_stats, q_adaptive, linewidth=1.1, label="adaptive target")
    axes[2].plot(t_stats, q_buffered, linewidth=1.0, label="buffered frames")
    axes[2].set_ylabel("frames")
    axes[2].set_title("Controller Depth Response")
    axes[2].grid(True, alpha=0.25)
    axes[2].legend(loc="upper right")

    if ideal_t:
        axes[3].plot(ideal_t, ideal_latency, linewidth=1.4, label="ideal latency budget = delay + 2*jitter")
    axes[3].plot(t_arr, latency_mean, linewidth=1.0, label="actual latency rolling mean")
    axes[3].plot(t_arr, latency_p95, linewidth=1.0, label="actual latency rolling p95")
    axes[3].set_xlabel("time (s)")
    axes[3].set_ylabel("ms")
    axes[3].set_title("Final User-Visible Effect")
    axes[3].grid(True, alpha=0.25)
    axes[3].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "network_vs_controller.png")

    fig, axes = plt.subplots(3, 1, figsize=(18, 12), sharex=True)
    if ideal_t:
        axes[0].plot(ideal_t, ideal_latency, linewidth=1.5, label="ideal latency budget")
    axes[0].plot(t_arr, latencies, linewidth=0.5, alpha=0.22, label="actual latency samples")
    axes[0].plot(t_arr, latency_p95, linewidth=1.2, label="actual rolling p95")
    axes[0].set_ylabel("ms")
    axes[0].set_title("Ideal Controller vs Actual: Latency")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].plot(t_stats, q_expected_raw, linewidth=1.1, label="ideal depth need from estimator")
    axes[1].plot(t_stats, q_adaptive, linewidth=1.1, label="actual adaptive target")
    axes[1].plot(t_stats, q_buffered, linewidth=1.0, label="actual buffered")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Ideal Controller vs Actual: Buffering")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")

    axes[2].plot(t_stats, q_headroom, linewidth=1.0, label="headroom = adaptive - buffered")
    axes[2].plot(t_stats, q_bad, linewidth=1.0, label="visible failures")
    axes[2].axhline(0.0, color="black", linewidth=1.0, alpha=0.45)
    axes[2].set_xlabel("time (s)")
    axes[2].set_ylabel("frames / count")
    axes[2].set_title("Ideal Controller vs Actual: Residual Cost")
    axes[2].grid(True, alpha=0.25)
    axes[2].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "latency_ideal_vs_actual.png")

    fig, axes = plt.subplots(2, 1, figsize=(18, 11), sharex=True)
    axes[0].stackplot(
        t_stats,
        q_reorder_for_explain,
        q_jitter_component,
        [DEPTH_MARGIN_FRAMES for _ in t_stats],
        q_pressure_component,
        labels=("reorder/forward-gap", "{} x tail jitter".format(fmt(JITTER_WEIGHT, 1)), "margin", "pressure"),
        alpha=0.72,
    )
    axes[0].plot(t_stats, q_raw, color="black", linewidth=1.1, label="raw target")
    axes[0].set_ylabel("frames")
    axes[0].set_title("Control target parameter contributions")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    for weight in (1.0, 2.0, 3.0):
        target = [d + weight * j + DEPTH_MARGIN_FRAMES for d, j in zip(q_reorder_for_explain, q_tail_jitter_frames)]
        axes[1].plot(t_stats, target, linewidth=1.0, label="jitter_weight={}".format(fmt(weight, 1)))
    axes[1].plot(t_stats, q_adaptive, color="black", linewidth=1.2, label="actual adaptive target")
    axes[1].set_xlabel("time (s)")
    axes[1].set_ylabel("frames")
    axes[1].set_title("Jitter-weight sensitivity")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    annotate_stages(axes, stages)
    save_figure(fig, out_dir, "parameter_influence.png")

    latency_stats = percentile_values(latencies)
    raw_stats = percentile_values(q_raw)
    adaptive_stats = percentile_values(q_adaptive)
    buffered_stats = percentile_values(q_buffered)
    overhead_stats = percentile_values(q_depth_overhead)
    response_stats = percentile_values(q_response_ratio)
    tracking_error_abs_stats = percentile_values([abs(v) for v in q_tracking_error])
    pacing_stats = percentile_values(q_pace)
    tail_frame_stats = percentile_values(q_tail_jitter_frames)
    disorder_stats = percentile_values(q_disorder)
    residual_stats = percentile_values(q_raw_residual)
    pressure_component_stats = percentile_values(q_pressure_component)
    reorder_component_stats = percentile_values(q_reorder_for_explain)
    jitter_component_stats = percentile_values(q_jitter_component)
    headroom_sorted = sorted(q_headroom)

    counters = {
        "skip": total(q_skip),
        "late": total(q_late),
        "overflow": total(q_ovf),
        "drop": total(q_drop),
        "stale": total(q_stale),
        "reorder": total(q_reorder),
    }
    bad_event_rate = total(q_bad) / float(len(q_bad)) if q_bad else 0.0

    verdict = []
    if counters["skip"] == 0 and counters["late"] == 0 and counters["overflow"] == 0:
        verdict.append("PASS: no skip/late/overflow events were observed.")
    else:
        verdict.append("ATTENTION: skip={}, late={}, overflow={}.".format(
            int(counters["skip"]), int(counters["late"]), int(counters["overflow"])
        ))

    if residual_stats["p95"] <= 1.0:
        verdict.append("PASS: raw depth is mostly explained by tail jitter, reorder, and margin.")
    else:
        verdict.append("WATCH: raw depth has residual beyond visible jitter/reorder; pressure or internal guards may be active.")

    headroom_p05 = percentile(headroom_sorted, 0.05) if headroom_sorted else 0.0
    allowed_headroom_debt = max(2.0, adaptive_stats["p95"] * 0.5)
    if headroom_p05 >= -allowed_headroom_debt or (counters["skip"] == 0 and counters["late"] == 0 and counters["overflow"] == 0):
        verdict.append("PASS: buffer occupancy is acceptable for the observed adaptive depth.")
    else:
        verdict.append("ATTENTION: buffered frames often exceed adaptive depth by a large margin.")

    if adaptive_stats["p95"] <= max(4.0, raw_stats["p95"] + 2.0):
        verdict.append("PASS: adaptive depth stays reasonably close to the estimated need.")
    else:
        verdict.append("ATTENTION: adaptive depth is much higher than raw depth; decay may be too conservative.")

    stability_score = 100.0 - min(60.0, total(q_bad) * 10.0) - min(20.0, counters["drop"] * 0.5)
    response_score = 100.0 - min(30.0, residual_stats["p95"] * 10.0) - min(25.0, max(0.0, overhead_stats["p95"] - 2.0) * 6.0)
    cost_score = 100.0 - min(35.0, max(0.0, adaptive_stats["p95"] - 8.0) * 5.0) - min(
        20.0, max(0.0, latency_stats["p95"] - latency_stats["p50"]) * 1.5
    )
    score = clamp(0.45 * stability_score + 0.30 * response_score + 0.25 * cost_score, 0.0, 100.0)

    if counters["skip"] == 0 and counters["late"] == 0 and counters["overflow"] == 0:
        summary = "stable delivery; judge remaining tradeoff by latency and adaptive-depth cost"
    else:
        summary = "visible controller failures observed; inspect bad-event samples first"

    event_peaks = top_rows(t_stats, q_bad)
    depth_peaks = top_rows(t_stats, q_adaptive)
    residual_peaks = top_rows(t_stats, q_raw_residual)
    stage_effects = stage_effect_rows(
        stages,
        t_stats,
        t_arr,
        {
            "q_tail_jitter_ms": q_tail_jitter,
            "q_adaptive_depth": q_adaptive,
            "q_buffered_frames": q_buffered,
            "q_pacing_factor": q_pace,
            "q_bad": q_bad,
            "latency_ms": latencies,
        },
    )

    write_assessment(
        out_dir,
        {
            "score": score,
            "summary": summary,
            "sample_count": len(stats),
            "arrival_count": len(arrivals),
            "verdict": verdict,
            "counters": counters,
            "stability": {
                "bad_event_rate": bad_event_rate,
            },
            "responsiveness": {
                "response_ratio_p95": response_stats["p95"],
                "tracking_error_abs_p95": tracking_error_abs_stats["p95"],
                "pacing_factor_mean": pacing_stats["mean"],
                "pacing_factor_p95": pacing_stats["p95"],
            },
            "events": event_peaks,
            "depth_peaks": depth_peaks,
            "residual_peaks": residual_peaks,
            "stage_effects": stage_effects,
            "latency": latency_stats,
            "depth": {
                "raw_mean": raw_stats["mean"],
                "raw_p95": raw_stats["p95"],
                "raw_max": raw_stats["max"],
                "adaptive_mean": adaptive_stats["mean"],
                "adaptive_p95": adaptive_stats["p95"],
                "adaptive_max": adaptive_stats["max"],
                "depth_overhead_mean": overhead_stats["mean"],
                "depth_overhead_p95": overhead_stats["p95"],
                "headroom_mean": mean(q_headroom),
                "headroom_p05": headroom_p05,
                "headroom_min": min(q_headroom) if q_headroom else 0.0,
                "buffered_mean": buffered_stats["mean"],
                "buffered_p95": buffered_stats["p95"],
                "buffered_max": buffered_stats["max"],
            },
            "explain": {
                "tail_jitter_frames_p95": tail_frame_stats["p95"],
                "disorder_frames_p95": disorder_stats["p95"],
                "raw_residual_mean": residual_stats["mean"],
                "raw_residual_p95": residual_stats["p95"],
                "raw_residual_max": residual_stats["max"],
                "pressure_component_p95": pressure_component_stats["p95"],
            },
            "influence": {
                "reorder_mean": reorder_component_stats["mean"],
                "jitter_mean": jitter_component_stats["mean"],
                "jitter_p95": jitter_component_stats["p95"],
            },
        },
    )


if __name__ == "__main__":
    main()
