#!/usr/bin/env python3
"""
Orochi DB Benchmark Results Aggregator

Aggregates benchmark results from multiple runs and generates
consolidated reports with statistics and comparisons.
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Any
import statistics

class BenchmarkResult:
    """Represents a single benchmark result."""

    def __init__(self, data: Dict[str, Any]):
        self.name = data.get('name', 'unknown')
        self.success = data.get('success', False)
        self.total_queries = data.get('total_queries', 0)
        self.failed_queries = data.get('failed_queries', 0)
        self.total_rows = data.get('total_rows', 0)
        self.latency = data.get('latency', {})
        self.throughput = data.get('throughput', {})
        self.timestamp = data.get('timestamp', datetime.now().isoformat())
        self.metadata = data.get('metadata', {})

    @property
    def avg_latency_ms(self) -> float:
        return self.latency.get('avg_us', 0) / 1000.0

    @property
    def p95_latency_ms(self) -> float:
        return self.latency.get('p95_us', 0) / 1000.0

    @property
    def p99_latency_ms(self) -> float:
        return self.latency.get('p99_us', 0) / 1000.0

    @property
    def ops_per_sec(self) -> float:
        return self.throughput.get('ops_per_sec', 0)

    @property
    def rows_per_sec(self) -> float:
        return self.throughput.get('rows_per_sec', 0)


class BenchmarkSuite:
    """Represents a benchmark suite with multiple results."""

    def __init__(self, data: Dict[str, Any]):
        self.name = data.get('name', 'unknown')
        self.version = data.get('version', '1.0')
        self.timestamp = data.get('timestamp', datetime.now().isoformat())
        self.config = data.get('config', {})
        self.system_info = data.get('system_info', {})
        self.results = [BenchmarkResult(r) for r in data.get('results', [])]

    @property
    def total_benchmarks(self) -> int:
        return len(self.results)

    @property
    def successful_benchmarks(self) -> int:
        return sum(1 for r in self.results if r.success)

    @property
    def failed_benchmarks(self) -> int:
        return sum(1 for r in self.results if not r.success)


class ResultsAggregator:
    """Aggregates and analyzes benchmark results."""

    def __init__(self, results_dir: str):
        self.results_dir = Path(results_dir)
        self.suites: List[BenchmarkSuite] = []
        self.comparisons: Dict[str, List[BenchmarkResult]] = {}

    def load_results(self, pattern: str = "**/*.json") -> int:
        """Load all JSON result files matching the pattern."""
        count = 0
        for result_file in self.results_dir.glob(pattern):
            try:
                with open(result_file, 'r') as f:
                    data = json.load(f)

                # Handle both suite format and individual result format
                if 'results' in data:
                    suite = BenchmarkSuite(data)
                    self.suites.append(suite)

                    # Index results by name for comparison
                    for result in suite.results:
                        if result.name not in self.comparisons:
                            self.comparisons[result.name] = []
                        self.comparisons[result.name].append(result)

                count += 1
            except (json.JSONDecodeError, KeyError) as e:
                print(f"Warning: Failed to load {result_file}: {e}", file=sys.stderr)

        return count

    def get_summary(self) -> Dict[str, Any]:
        """Generate a summary of all loaded results."""
        if not self.suites:
            return {"error": "No results loaded"}

        total_benchmarks = sum(s.total_benchmarks for s in self.suites)
        successful = sum(s.successful_benchmarks for s in self.suites)
        failed = sum(s.failed_benchmarks for s in self.suites)

        # Aggregate latencies
        all_latencies = []
        all_throughputs = []
        for suite in self.suites:
            for result in suite.results:
                if result.success:
                    all_latencies.append(result.avg_latency_ms)
                    all_throughputs.append(result.ops_per_sec)

        return {
            "total_suites": len(self.suites),
            "total_benchmarks": total_benchmarks,
            "successful": successful,
            "failed": failed,
            "success_rate": successful / total_benchmarks * 100 if total_benchmarks > 0 else 0,
            "latency_stats": {
                "min_ms": min(all_latencies) if all_latencies else 0,
                "max_ms": max(all_latencies) if all_latencies else 0,
                "avg_ms": statistics.mean(all_latencies) if all_latencies else 0,
                "median_ms": statistics.median(all_latencies) if all_latencies else 0,
            },
            "throughput_stats": {
                "min_ops": min(all_throughputs) if all_throughputs else 0,
                "max_ops": max(all_throughputs) if all_throughputs else 0,
                "avg_ops": statistics.mean(all_throughputs) if all_throughputs else 0,
            }
        }

    def get_comparison(self, benchmark_name: str) -> Dict[str, Any]:
        """Get comparison data for a specific benchmark across runs."""
        results = self.comparisons.get(benchmark_name, [])

        if not results:
            return {"error": f"No results found for {benchmark_name}"}

        latencies = [r.avg_latency_ms for r in results if r.success]
        throughputs = [r.ops_per_sec for r in results if r.success]

        return {
            "name": benchmark_name,
            "runs": len(results),
            "successful_runs": len(latencies),
            "latency": {
                "values_ms": latencies,
                "min_ms": min(latencies) if latencies else 0,
                "max_ms": max(latencies) if latencies else 0,
                "avg_ms": statistics.mean(latencies) if latencies else 0,
                "stddev_ms": statistics.stdev(latencies) if len(latencies) > 1 else 0,
            },
            "throughput": {
                "values_ops": throughputs,
                "min_ops": min(throughputs) if throughputs else 0,
                "max_ops": max(throughputs) if throughputs else 0,
                "avg_ops": statistics.mean(throughputs) if throughputs else 0,
                "stddev_ops": statistics.stdev(throughputs) if len(throughputs) > 1 else 0,
            }
        }

    def get_benchmark_categories(self) -> Dict[str, List[str]]:
        """Group benchmarks by category based on naming convention."""
        categories = {
            "tpch": [],
            "timeseries": [],
            "columnar": [],
            "distributed": [],
            "multitenant": [],
            "sharding": [],
            "connection": [],
            "other": []
        }

        for name in self.comparisons.keys():
            categorized = False
            for cat in categories:
                if cat in name.lower():
                    categories[cat].append(name)
                    categorized = True
                    break
            if not categorized:
                categories["other"].append(name)

        # Remove empty categories
        return {k: v for k, v in categories.items() if v}

    def export_summary_json(self, output_file: str):
        """Export summary to JSON file."""
        summary = {
            "generated_at": datetime.now().isoformat(),
            "summary": self.get_summary(),
            "categories": self.get_benchmark_categories(),
            "benchmarks": {
                name: self.get_comparison(name)
                for name in self.comparisons.keys()
            }
        }

        with open(output_file, 'w') as f:
            json.dump(summary, f, indent=2)

        print(f"Summary exported to {output_file}")


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Aggregate benchmark results')
    parser.add_argument('--results-dir', '-r', default='results',
                        help='Directory containing result JSON files')
    parser.add_argument('--output', '-o', default='summary.json',
                        help='Output summary file')
    parser.add_argument('--pattern', '-p', default='**/*.json',
                        help='Glob pattern for result files')

    args = parser.parse_args()

    aggregator = ResultsAggregator(args.results_dir)
    count = aggregator.load_results(args.pattern)

    print(f"Loaded {count} result files")

    if count > 0:
        summary = aggregator.get_summary()
        print(f"\nSummary:")
        print(f"  Total Suites: {summary['total_suites']}")
        print(f"  Total Benchmarks: {summary['total_benchmarks']}")
        print(f"  Success Rate: {summary['success_rate']:.1f}%")
        print(f"  Avg Latency: {summary['latency_stats']['avg_ms']:.2f}ms")
        print(f"  Avg Throughput: {summary['throughput_stats']['avg_ops']:.0f} ops/sec")

        aggregator.export_summary_json(args.output)


if __name__ == '__main__':
    main()
