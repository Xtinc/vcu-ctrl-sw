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


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


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


def save_figure(fig, out_dir, filename):
    fig.tight_layout()
    output_path = os.path.join(out_dir, filename)
    fig.savefig(output_path, dpi=SAVE_DPI)
    plt.close(fig)
    print("wrote {}".format(output_path))


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
                    "raw_residual_max"):
            f.write("- {}: {}\n".format(key, fmt(metrics["explain"].get(key, 0.0))))
        if metrics["residual_peaks"]:
            f.write("- highest unexplained-raw samples:\n")
            for item in metrics["residual_peaks"]:
                f.write("  t={}s, residual={}\n".format(fmt(item[0]), fmt(item[1])))
    print("wrote {}".format(output_path))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 tests/plot_reliable_udp_jitter.py <out-dir>")

    out_dir = sys.argv[1]
    arrival_path = os.path.join(out_dir, "arrival_intervals.csv")
    stats_path = os.path.join(out_dir, "queue_stats.csv")
    require_file(arrival_path)
    require_file(stats_path)

    arrivals = read_csv(arrival_path)
    stats = read_csv(stats_path)
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

    q_drop = [as_float(r, "q_drop_delta") for r in stats]
    q_late = [as_float(r, "q_late_delta") for r in stats]
    q_reorder = [as_float(r, "q_reorder_delta") for r in stats]
    q_skip = [as_float(r, "q_skip_delta") for r in stats]
    q_ovf = [as_float(r, "q_ovf_delta") for r in stats]
    q_stale = [as_float(r, "q_stale_delta") for r in stats]

    q_reorder_for_explain = [max(d, m) for d, m in zip(q_disorder, q_max_disorder)]
    q_expected_raw = [
        d + JITTER_WEIGHT * j + DEPTH_MARGIN_FRAMES for d, j in zip(q_reorder_for_explain, q_tail_jitter_frames)
    ]
    q_raw_residual = [max(0.0, raw - expected) for raw, expected in zip(q_raw, q_expected_raw)]
    q_headroom = [adaptive - buffered for adaptive, buffered in zip(q_adaptive, q_buffered)]
    q_depth_overhead = [max(0.0, adaptive - raw) for adaptive, raw in zip(q_adaptive, q_raw)]
    q_response_ratio = [
        adaptive / max(1.0, math.ceil(raw)) if raw > 0.0 else adaptive for adaptive, raw in zip(q_adaptive, q_raw)
    ]
    q_bad = [skip + late + ovf for skip, late, ovf in zip(q_skip, q_late, q_ovf)]

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.scatter(t_arr, intervals, s=SCATTER_SIZE, alpha=0.35, label="arrival interval")
    ax.scatter(t_arr, interval_mean, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="rolling mean")
    ax.scatter(t_arr, interval_p95, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="rolling p95")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("ms")
    ax.set_title("Business-frame arrival interval")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right")
    save_figure(fig, out_dir, "arrival_intervals.png")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.scatter(t_arr, latencies, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, color="tab:green")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("ms")
    ax.set_title("End-to-end latency")
    ax.grid(True, alpha=0.25)
    save_figure(fig, out_dir, "latency.png")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.scatter(t_stats, q_avg_fi, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="q_avg_fi_ms")
    ax.scatter(t_stats, q_jitter, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="q_jitter_ms")
    ax.scatter(t_stats, q_tail_jitter, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="q_tail_jitter_ms")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("ms")
    ax.set_title("RecvQueue timing estimates")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right")
    save_figure(fig, out_dir, "queue_timing.png")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.scatter(t_stats, q_buffered, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="buffered")
    ax.scatter(t_stats, q_adaptive, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="adaptive depth")
    ax.scatter(t_stats, q_raw, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="raw depth")
    ax.scatter(t_stats, q_expected_raw, s=SCATTER_SIZE, alpha=0.45, label="expected raw")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("frames")
    ax.set_title("RecvQueue depth control")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right")
    save_figure(fig, out_dir, "queue_depth.png")

    fig, axes = plt.subplots(3, 1, figsize=(18, 12), sharex=True)
    axes[0].scatter(t_stats, q_raw, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="raw depth")
    axes[0].scatter(t_stats, q_expected_raw, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="expected raw")
    axes[0].scatter(t_stats, q_raw_residual, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="unexplained raw")
    axes[0].set_ylabel("frames")
    axes[0].set_title("Depth estimate explanation")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].scatter(t_stats, q_headroom, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="adaptive - buffered")
    axes[1].axhline(0.0, color="black", linewidth=1.0, alpha=0.45)
    axes[1].set_ylabel("frames")
    axes[1].set_title("Buffer headroom")
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")

    axes[2].scatter(t_stats, q_bad, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="skip + late + overflow")
    axes[2].scatter(t_stats, q_reorder, s=SCATTER_SIZE, alpha=0.45, label="reorder")
    axes[2].set_xlabel("time (s)")
    axes[2].set_ylabel("delta count")
    axes[2].set_title("Controller failures vs network reorder")
    axes[2].grid(True, alpha=0.25)
    axes[2].legend(loc="upper right")
    save_figure(fig, out_dir, "controller_assessment.png")

    fig, axes = plt.subplots(2, 2, figsize=(18, 10), sharex=True)
    axes[0][0].scatter(t_stats, q_bad, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="bad events")
    axes[0][0].scatter(t_stats, q_late, s=SCATTER_SIZE, alpha=0.45, label="late")
    axes[0][0].scatter(t_stats, q_skip, s=SCATTER_SIZE, alpha=0.45, label="skip")
    axes[0][0].set_title("Stability: visible failures")
    axes[0][0].set_ylabel("delta count")
    axes[0][0].grid(True, alpha=0.25)
    axes[0][0].legend(loc="upper right")

    axes[0][1].scatter(t_stats, q_adaptive, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="adaptive")
    axes[0][1].scatter(t_stats, q_raw, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="raw")
    axes[0][1].scatter(t_stats, q_depth_overhead, s=SCATTER_SIZE, alpha=0.45, label="adaptive - raw")
    axes[0][1].set_title("Responsiveness: target tracking")
    axes[0][1].set_ylabel("frames")
    axes[0][1].grid(True, alpha=0.25)
    axes[0][1].legend(loc="upper right")

    axes[1][0].scatter(t_arr, latencies, s=SCATTER_SIZE, alpha=0.35, label="latency")
    axes[1][0].axhline(percentile_values(latencies)["p95"], color="tab:red", linewidth=1.0, alpha=0.65, label="P95")
    axes[1][0].set_title("Cost: end-to-end latency")
    axes[1][0].set_xlabel("time (s)")
    axes[1][0].set_ylabel("ms")
    axes[1][0].grid(True, alpha=0.25)
    axes[1][0].legend(loc="upper right")

    axes[1][1].scatter(t_stats, q_expected_raw, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="expected raw")
    axes[1][1].scatter(t_stats, q_raw_residual, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="unexplained raw")
    axes[1][1].set_title("Explainability: raw-depth decomposition")
    axes[1][1].set_xlabel("time (s)")
    axes[1][1].set_ylabel("frames")
    axes[1][1].grid(True, alpha=0.25)
    axes[1][1].legend(loc="upper right")
    save_figure(fig, out_dir, "controller_dashboard.png")

    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.scatter(t_stats, q_drop, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="drop")
    ax.scatter(t_stats, q_late, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="late")
    ax.scatter(t_stats, q_reorder, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="reorder")
    ax.scatter(t_stats, q_skip, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="skip")
    ax.scatter(t_stats, q_ovf, s=SCATTER_SIZE, alpha=SCATTER_ALPHA, label="overflow")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("delta count")
    ax.set_title("RecvQueue counter deltas")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper right")
    save_figure(fig, out_dir, "queue_counters.png")

    latency_stats = percentile_values(latencies)
    raw_stats = percentile_values(q_raw)
    adaptive_stats = percentile_values(q_adaptive)
    buffered_stats = percentile_values(q_buffered)
    overhead_stats = percentile_values(q_depth_overhead)
    response_stats = percentile_values(q_response_ratio)
    tail_frame_stats = percentile_values(q_tail_jitter_frames)
    disorder_stats = percentile_values(q_disorder)
    residual_stats = percentile_values(q_raw_residual)
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
            },
            "events": event_peaks,
            "depth_peaks": depth_peaks,
            "residual_peaks": residual_peaks,
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
            },
        },
    )


if __name__ == "__main__":
    main()
