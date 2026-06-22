#!/usr/bin/env python3
import csv
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


if __name__ == "__main__":
    unittest.main()
