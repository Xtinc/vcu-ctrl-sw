#!/usr/bin/env python3
import csv
import glob
import math
import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from lib_network import plot_queue_stats as queue_core
import plot


class PlotContractTest(unittest.TestCase):
    def write_csv(self, directory, name, header, rows):
        path = os.path.join(directory, name)
        with open(path, "w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(header)
            writer.writerows(rows)
        return path

    def make_valid_enhancements(self, directory):
        self.write_csv(directory, "capture_meta.csv", plot.META_COLUMNS,
                       [[plot.CONTRACT_VERSION, "network"]])
        self.write_csv(directory, "input_intervals.csv", plot.INPUT_COLUMNS, [[0.1, 0.0], [0.2, 100.0]])
        self.write_csv(directory, "arrival_intervals.csv", plot.ARRIVAL_COLUMNS,
                       [[0.2, 1, 0.0, "", 800, 0], [0.3, 2, 100.0, 5.0, 800, 1]])

    def queue_row(self, timestamp, segment=1, **values):
        row = {key: 0 for key in queue_core.QUEUE_HEADER}
        row.update({"timestamp_utc": timestamp, "session_id": 1000, "segment_id": segment,
                    "q_short_fi_ms": 2.0, "q_avg_fi_ms": 2.0, "q_out_fi_ms": 4.0,
                    "q_buffered_frames": 2, "q_adaptive_depth": 3, "q_depth_raw": 2.5})
        row.update(values)
        return [row[key] for key in queue_core.QUEUE_HEADER]

    def make_product_capture(self, directory):
        rows = [
            self.queue_row("2026-06-22T00:00:00.000000Z", q_jitter_ms=1.0, q_tail_jitter_ms=3.0,
                           q_pressure_frames=0, q_recv_delta=10, q_dlv_delta=8, q_skip_delta=1),
            self.queue_row("2026-06-22T00:00:05.000000Z", q_out_fi_ms=5.0, q_buffered_frames=4,
                           q_adaptive_depth=5, q_depth_raw=4.5, q_jitter_ms=2.0, q_tail_jitter_ms=6.0,
                           q_pressure_frames=2, q_recv_delta=20, q_dlv_delta=19, q_dup_delta=2),
            self.queue_row("2026-06-22T00:00:12.000000Z", segment=2, idle_gap_s=7.0, q_avg_fi_ms=0.0,
                           q_short_fi_ms=0.0, q_out_fi_ms=0.0, q_drop_delta=1, q_ovf_delta=1),
        ]
        return self.write_csv(directory, "queue_stats.csv", queue_core.QUEUE_HEADER, rows)

    def test_enhancement_contract_accepts_exact_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_valid_enhancements(directory)
            self.assertEqual(("network", 0.1), plot.validate_capture(directory))

    def test_enhancement_contract_rejects_non_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_valid_enhancements(directory)
            self.write_csv(directory, "input_intervals.csv", plot.INPUT_COLUMNS, [[0.2, 0.0], [0.1, 1.0]])
            with self.assertRaises(SystemExit):
                plot.process_arrivals(os.path.join(directory, "arrival_intervals.csv"),
                                      os.path.join(directory, "input_intervals.csv"), [], [], 10.0,
                                      lambda *args: None)

    def test_enhancement_contract_rejects_extra_column(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_valid_enhancements(directory)
            self.write_csv(directory, "input_intervals.csv", plot.INPUT_COLUMNS + ("extra",), [[0.1, 0.0, 1]])
            with self.assertRaises(SystemExit):
                plot.validate_capture(directory)

    def test_enhancement_contract_rejects_wrong_version(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_valid_enhancements(directory)
            self.write_csv(directory, "capture_meta.csv", plot.META_COLUMNS, [["old_contract", "network"]])
            with self.assertRaises(SystemExit):
                plot.validate_capture(directory)

    def test_enhancement_contract_rejects_non_finite_value(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_valid_enhancements(directory)
            self.write_csv(directory, "input_intervals.csv", plot.INPUT_COLUMNS, [[0.1, "nan"]])
            with self.assertRaises(SystemExit):
                plot.process_arrivals(os.path.join(directory, "arrival_intervals.csv"),
                                      os.path.join(directory, "input_intervals.csv"), [], [], 10.0,
                                      lambda *args: None)

    def test_product_contract_rejects_old_header(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_csv(directory, "queue_stats.csv", ("elapsed_s",), [[0.0]])
            with self.assertRaises(queue_core.CSVContractError):
                list(queue_core.iter_queue_rows(path))

    def test_product_metrics_and_estimated_delays(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_product_capture(directory)
            queue = queue_core.process_queue(path)
            self.assertEqual([8.0, 20.0, 0.0],
                             [value for value in queue["plot"]["estimated_current_delay_ms"]
                              if value == value])
            self.assertEqual([12.0, 25.0, 0.0],
                             [value for value in queue["plot"]["estimated_target_delay_ms"]
                              if value == value])
            self.assertAlmostEqual(queue["pressure_ratio"], 1.0 / 3.0)
            self.assertEqual(queue["counters"]["recv"], 30)
            self.assertEqual(queue["counters"]["dlv"], 27)
            self.assertEqual(queue["counters"]["dup"], 2)
            self.assertEqual(queue["counters"]["overflow"], 1)
            self.assertTrue(all(math.isfinite(value) for value in queue["plot"]["q_tail_jitter_frames"]
                                if not math.isnan(value)))

    def test_product_paging_and_report_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            queue = queue_core.process_queue(self.make_product_capture(directory))
            self.assertEqual([(0.0, 10.0), (10.0, 20.0)], queue_core.window_ranges(queue, 10.0))
            windows = [queue_core.queue_plot_window(queue, start, end)
                       for start, end in queue_core.window_ranges(queue, 10.0)]
            self.assertEqual(3, sum(sum(value == value for value in window["q_avg_fi_ms"])
                                    for window in windows))
            queue_core.render_product_outputs(queue, directory, 10.0)
            self.assertEqual(2, len(glob.glob(os.path.join(directory, "queue_timing_*.png"))))
            self.assertEqual(2, len(glob.glob(os.path.join(directory, "queue_latency_*.png"))))
            self.assertEqual(2, len(glob.glob(os.path.join(directory, "queue_network_*.png"))))
            with open(os.path.join(directory, "queue_report.txt")) as stream:
                report = stream.read()
            self.assertIn("Estimated jitter-buffer delay", report)
            self.assertIn("not end-to-end latency", report)
            self.assertIn("Unavailable from product queue stats", report)


if __name__ == "__main__":
    unittest.main()
