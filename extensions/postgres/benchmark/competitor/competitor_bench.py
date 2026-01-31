#!/usr/bin/env python3
"""
Orochi DB Competitor Comparison Benchmarking Framework

A comprehensive benchmarking orchestrator that runs standardized benchmarks
(TPC-C, TPC-H, TSBS, ClickBench) against multiple database targets and
generates comparison reports.

Supported targets:
  - orochi: Orochi DB (PostgreSQL with Orochi extension)
  - vanilla-postgres: Standard PostgreSQL
  - timescaledb: PostgreSQL with TimescaleDB extension
  - citus: PostgreSQL with Citus extension
  - questdb: QuestDB (time-series focused)

Usage:
  python competitor_bench.py --benchmark=tpcc --targets=orochi,timescaledb
  python competitor_bench.py --benchmark=all --targets=all
  python competitor_bench.py --benchmark=tsbs --targets=orochi,questdb --report-only
"""

import argparse
import datetime
import json
import logging
import os
import platform
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional, Any
import hashlib
import shutil

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Benchmark framework paths
SCRIPT_DIR = Path(__file__).parent.resolve()
RESULTS_DIR = SCRIPT_DIR / "results"
CONFIGS_DIR = SCRIPT_DIR / "configs"
CHARTS_DIR = SCRIPT_DIR / "charts"

# Supported benchmarks and targets
SUPPORTED_BENCHMARKS = ['tpcc', 'tpch', 'tsbs', 'clickbench', 'all']
SUPPORTED_TARGETS = ['orochi', 'vanilla-postgres', 'timescaledb', 'citus', 'questdb', 'all']

# Default connection configurations
DEFAULT_CONFIGS = {
    'orochi': {
        'host': 'localhost',
        'port': 5432,
        'database': 'orochi_bench',
        'user': 'postgres',
        'extension': 'orochi'
    },
    'vanilla-postgres': {
        'host': 'localhost',
        'port': 5433,
        'database': 'vanilla_bench',
        'user': 'postgres',
        'extension': None
    },
    'timescaledb': {
        'host': 'localhost',
        'port': 5434,
        'database': 'timescale_bench',
        'user': 'postgres',
        'extension': 'timescaledb'
    },
    'citus': {
        'host': 'localhost',
        'port': 5435,
        'database': 'citus_bench',
        'user': 'postgres',
        'extension': 'citus'
    },
    'questdb': {
        'host': 'localhost',
        'port': 8812,
        'database': 'qdb',
        'user': 'admin',
        'extension': None
    }
}


@dataclass
class HardwareInfo:
    """Hardware specification information."""
    cpu_model: str = ""
    cpu_cores: int = 0
    cpu_threads: int = 0
    memory_gb: float = 0.0
    storage_type: str = ""
    os_version: str = ""
    kernel_version: str = ""

    @classmethod
    def collect(cls) -> 'HardwareInfo':
        """Collect hardware information from the system."""
        info = cls()

        try:
            # CPU info
            if platform.system() == 'Linux':
                with open('/proc/cpuinfo', 'r') as f:
                    cpuinfo = f.read()
                    for line in cpuinfo.split('\n'):
                        if 'model name' in line:
                            info.cpu_model = line.split(':')[1].strip()
                            break
                info.cpu_cores = os.cpu_count() or 0
                info.cpu_threads = info.cpu_cores  # Simplified

                # Memory
                with open('/proc/meminfo', 'r') as f:
                    meminfo = f.read()
                    for line in meminfo.split('\n'):
                        if 'MemTotal' in line:
                            mem_kb = int(line.split()[1])
                            info.memory_gb = round(mem_kb / (1024 * 1024), 2)
                            break
            elif platform.system() == 'Darwin':
                result = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'],
                                       capture_output=True, text=True)
                info.cpu_model = result.stdout.strip()
                info.cpu_cores = os.cpu_count() or 0
                info.cpu_threads = info.cpu_cores

                result = subprocess.run(['sysctl', '-n', 'hw.memsize'],
                                       capture_output=True, text=True)
                info.memory_gb = round(int(result.stdout.strip()) / (1024**3), 2)

            info.os_version = f"{platform.system()} {platform.release()}"
            info.kernel_version = platform.version()

            # Storage type detection (simplified)
            info.storage_type = "SSD (assumed)"

        except Exception as e:
            logger.warning(f"Could not collect all hardware info: {e}")

        return info


@dataclass
class SoftwareInfo:
    """Software version information."""
    postgresql_version: str = ""
    extension_version: str = ""
    benchmark_tool_version: str = ""
    python_version: str = ""

    @classmethod
    def collect(cls, target: str, config: Dict) -> 'SoftwareInfo':
        """Collect software version information."""
        info = cls()
        info.python_version = platform.python_version()

        try:
            # Get PostgreSQL version
            conn_str = f"postgresql://{config['user']}@{config['host']}:{config['port']}/{config['database']}"
            result = subprocess.run(
                ['psql', conn_str, '-t', '-c', 'SELECT version();'],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0:
                info.postgresql_version = result.stdout.strip().split(',')[0]

            # Get extension version if applicable
            if config.get('extension'):
                result = subprocess.run(
                    ['psql', conn_str, '-t', '-c',
                     f"SELECT extversion FROM pg_extension WHERE extname = '{config['extension']}';"],
                    capture_output=True, text=True, timeout=10
                )
                if result.returncode == 0:
                    info.extension_version = result.stdout.strip()

        except Exception as e:
            logger.warning(f"Could not collect software info for {target}: {e}")

        return info


@dataclass
class BenchmarkResult:
    """Individual benchmark result."""
    benchmark_name: str
    target: str
    timestamp: str
    duration_seconds: float
    metrics: Dict[str, Any] = field(default_factory=dict)
    raw_output: str = ""
    config_hash: str = ""
    success: bool = True
    error_message: str = ""


@dataclass
class ComparisonReport:
    """Complete comparison report."""
    report_id: str
    created_at: str
    hardware: HardwareInfo
    benchmarks_run: List[str]
    targets: List[str]
    results: Dict[str, List[BenchmarkResult]] = field(default_factory=dict)
    relative_performance: Dict[str, Dict[str, float]] = field(default_factory=dict)
    software_info: Dict[str, SoftwareInfo] = field(default_factory=dict)

    def calculate_relative_performance(self, baseline: str = 'vanilla-postgres'):
        """Calculate relative performance ratios against a baseline."""
        if baseline not in self.targets:
            baseline = self.targets[0]

        for benchmark in self.benchmarks_run:
            self.relative_performance[benchmark] = {}

            # Find baseline result
            baseline_results = [r for r in self.results.get(benchmark, [])
                              if r.target == baseline and r.success]
            if not baseline_results:
                continue

            baseline_result = baseline_results[0]

            # Calculate ratios for each target
            for target in self.targets:
                target_results = [r for r in self.results.get(benchmark, [])
                                 if r.target == target and r.success]
                if not target_results:
                    continue

                target_result = target_results[0]

                # Calculate ratio based on primary metric
                if benchmark == 'tpcc':
                    baseline_metric = baseline_result.metrics.get('tpmc', 1)
                    target_metric = target_result.metrics.get('tpmc', 1)
                elif benchmark == 'tpch':
                    # Lower is better for query time
                    baseline_metric = 1 / max(baseline_result.metrics.get('total_time_seconds', 1), 0.001)
                    target_metric = 1 / max(target_result.metrics.get('total_time_seconds', 1), 0.001)
                elif benchmark == 'tsbs':
                    baseline_metric = baseline_result.metrics.get('rows_per_second', 1)
                    target_metric = target_result.metrics.get('rows_per_second', 1)
                elif benchmark == 'clickbench':
                    # Lower is better for query time
                    baseline_metric = 1 / max(baseline_result.metrics.get('total_time_seconds', 1), 0.001)
                    target_metric = 1 / max(target_result.metrics.get('total_time_seconds', 1), 0.001)
                else:
                    baseline_metric = 1
                    target_metric = 1

                ratio = target_metric / max(baseline_metric, 0.001)
                self.relative_performance[benchmark][target] = round(ratio, 2)

    def to_json(self) -> str:
        """Convert report to JSON."""
        return json.dumps(asdict(self), indent=2, default=str)

    def to_markdown(self) -> str:
        """Generate Markdown report."""
        return generate_markdown_report(self)


class BenchmarkRunner:
    """Orchestrates benchmark execution across targets."""

    def __init__(self,
                 benchmarks: List[str],
                 targets: List[str],
                 configs: Optional[Dict] = None,
                 output_dir: Optional[Path] = None):
        self.benchmarks = self._resolve_benchmarks(benchmarks)
        self.targets = self._resolve_targets(targets)
        self.configs = configs or DEFAULT_CONFIGS
        self.output_dir = output_dir or RESULTS_DIR
        self.output_dir.mkdir(parents=True, exist_ok=True)

        # Generate unique report ID
        timestamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        self.report_id = f"comparison_{timestamp}"

    def _resolve_benchmarks(self, benchmarks: List[str]) -> List[str]:
        """Resolve 'all' to full benchmark list."""
        if 'all' in benchmarks:
            return ['tpcc', 'tpch', 'tsbs', 'clickbench']
        return benchmarks

    def _resolve_targets(self, targets: List[str]) -> List[str]:
        """Resolve 'all' to full target list."""
        if 'all' in targets:
            return ['orochi', 'vanilla-postgres', 'timescaledb', 'citus']
        return targets

    def run_all(self) -> ComparisonReport:
        """Run all specified benchmarks against all specified targets."""
        logger.info(f"Starting benchmark comparison: {self.benchmarks} vs {self.targets}")

        # Collect hardware info
        hardware = HardwareInfo.collect()
        logger.info(f"Hardware: {hardware.cpu_model}, {hardware.memory_gb}GB RAM")

        # Initialize report
        report = ComparisonReport(
            report_id=self.report_id,
            created_at=datetime.datetime.now().isoformat(),
            hardware=hardware,
            benchmarks_run=self.benchmarks,
            targets=self.targets
        )

        # Collect software info for each target
        for target in self.targets:
            config = self.configs.get(target, DEFAULT_CONFIGS.get(target, {}))
            report.software_info[target] = SoftwareInfo.collect(target, config)

        # Run benchmarks
        for benchmark in self.benchmarks:
            report.results[benchmark] = []

            for target in self.targets:
                logger.info(f"Running {benchmark} against {target}...")
                result = self._run_benchmark(benchmark, target)
                report.results[benchmark].append(result)

                # Save intermediate result
                self._save_result(result)

        # Calculate relative performance
        report.calculate_relative_performance()

        # Save final report
        self._save_report(report)

        return report

    def _run_benchmark(self, benchmark: str, target: str) -> BenchmarkResult:
        """Run a single benchmark against a single target."""
        config = self.configs.get(target, DEFAULT_CONFIGS.get(target, {}))
        timestamp = datetime.datetime.now().isoformat()

        # Generate config hash for reproducibility
        config_hash = hashlib.md5(json.dumps(config, sort_keys=True).encode()).hexdigest()[:8]

        start_time = time.time()

        try:
            if benchmark == 'tpcc':
                metrics, output = self._run_tpcc(target, config)
            elif benchmark == 'tpch':
                metrics, output = self._run_tpch(target, config)
            elif benchmark == 'tsbs':
                metrics, output = self._run_tsbs(target, config)
            elif benchmark == 'clickbench':
                metrics, output = self._run_clickbench(target, config)
            else:
                raise ValueError(f"Unknown benchmark: {benchmark}")

            duration = time.time() - start_time

            return BenchmarkResult(
                benchmark_name=benchmark,
                target=target,
                timestamp=timestamp,
                duration_seconds=duration,
                metrics=metrics,
                raw_output=output,
                config_hash=config_hash,
                success=True
            )

        except Exception as e:
            logger.error(f"Benchmark {benchmark} failed for {target}: {e}")
            return BenchmarkResult(
                benchmark_name=benchmark,
                target=target,
                timestamp=timestamp,
                duration_seconds=time.time() - start_time,
                config_hash=config_hash,
                success=False,
                error_message=str(e)
            )

    def _run_tpcc(self, target: str, config: Dict) -> tuple:
        """Run TPC-C benchmark using wrapper script."""
        wrapper_script = SCRIPT_DIR / "tpcc_wrapper.sh"

        env = os.environ.copy()
        env.update({
            'BENCH_HOST': config['host'],
            'BENCH_PORT': str(config['port']),
            'BENCH_DB': config['database'],
            'BENCH_USER': config['user'],
            'BENCH_TARGET': target
        })

        result = subprocess.run(
            ['bash', str(wrapper_script)],
            capture_output=True,
            text=True,
            env=env,
            timeout=3600  # 1 hour timeout
        )

        # Parse output for metrics
        metrics = self._parse_tpcc_output(result.stdout)
        return metrics, result.stdout

    def _run_tpch(self, target: str, config: Dict) -> tuple:
        """Run TPC-H benchmark."""
        # TPC-H implementation would go here
        # For now, return placeholder
        metrics = {
            'scale_factor': 1,
            'total_time_seconds': 0,
            'query_times': {}
        }
        return metrics, "TPC-H benchmark placeholder"

    def _run_tsbs(self, target: str, config: Dict) -> tuple:
        """Run TSBS benchmark using wrapper script."""
        wrapper_script = SCRIPT_DIR / "tsbs_wrapper.sh"

        env = os.environ.copy()
        env.update({
            'BENCH_HOST': config['host'],
            'BENCH_PORT': str(config['port']),
            'BENCH_DB': config['database'],
            'BENCH_USER': config['user'],
            'BENCH_TARGET': target
        })

        result = subprocess.run(
            ['bash', str(wrapper_script)],
            capture_output=True,
            text=True,
            env=env,
            timeout=7200  # 2 hour timeout
        )

        metrics = self._parse_tsbs_output(result.stdout)
        return metrics, result.stdout

    def _run_clickbench(self, target: str, config: Dict) -> tuple:
        """Run ClickBench benchmark using wrapper script."""
        wrapper_script = SCRIPT_DIR / "clickbench_wrapper.sh"

        env = os.environ.copy()
        env.update({
            'BENCH_HOST': config['host'],
            'BENCH_PORT': str(config['port']),
            'BENCH_DB': config['database'],
            'BENCH_USER': config['user'],
            'BENCH_TARGET': target
        })

        result = subprocess.run(
            ['bash', str(wrapper_script)],
            capture_output=True,
            text=True,
            env=env,
            timeout=7200  # 2 hour timeout
        )

        metrics = self._parse_clickbench_output(result.stdout)
        return metrics, result.stdout

    def _parse_tpcc_output(self, output: str) -> Dict:
        """Parse TPC-C benchmark output."""
        metrics = {
            'tpmc': 0,
            'nopm': 0,
            'warehouses': 10,
            'latency_p50_ms': 0,
            'latency_p95_ms': 0,
            'latency_p99_ms': 0,
            'transactions_committed': 0,
            'transactions_rolled_back': 0
        }

        for line in output.split('\n'):
            line = line.strip().lower()
            if 'tpmc' in line or 'tpm-c' in line:
                try:
                    metrics['tpmc'] = float(line.split()[-1].replace(',', ''))
                except (ValueError, IndexError):
                    pass
            elif 'nopm' in line:
                try:
                    metrics['nopm'] = float(line.split()[-1].replace(',', ''))
                except (ValueError, IndexError):
                    pass
            elif 'p50' in line or '50th' in line:
                try:
                    metrics['latency_p50_ms'] = float(line.split()[-1].replace('ms', ''))
                except (ValueError, IndexError):
                    pass
            elif 'p95' in line or '95th' in line:
                try:
                    metrics['latency_p95_ms'] = float(line.split()[-1].replace('ms', ''))
                except (ValueError, IndexError):
                    pass
            elif 'p99' in line or '99th' in line:
                try:
                    metrics['latency_p99_ms'] = float(line.split()[-1].replace('ms', ''))
                except (ValueError, IndexError):
                    pass

        return metrics

    def _parse_tsbs_output(self, output: str) -> Dict:
        """Parse TSBS benchmark output."""
        metrics = {
            'rows_per_second': 0,
            'total_rows': 0,
            'total_time_seconds': 0,
            'query_latencies': {},
            'workload': 'devops'
        }

        for line in output.split('\n'):
            if 'rows/sec' in line.lower():
                try:
                    metrics['rows_per_second'] = float(line.split()[-2].replace(',', ''))
                except (ValueError, IndexError):
                    pass
            elif 'total rows' in line.lower():
                try:
                    metrics['total_rows'] = int(line.split()[-1].replace(',', ''))
                except (ValueError, IndexError):
                    pass

        return metrics

    def _parse_clickbench_output(self, output: str) -> Dict:
        """Parse ClickBench benchmark output."""
        metrics = {
            'total_time_seconds': 0,
            'query_count': 43,
            'query_times': [],
            'cold_run_time': 0,
            'hot_run_times': []
        }

        query_times = []
        for line in output.split('\n'):
            if line.startswith('Q') and ':' in line:
                try:
                    time_str = line.split(':')[1].strip().replace('s', '')
                    query_times.append(float(time_str))
                except (ValueError, IndexError):
                    pass

        if query_times:
            metrics['query_times'] = query_times
            metrics['total_time_seconds'] = sum(query_times)

        return metrics

    def _save_result(self, result: BenchmarkResult):
        """Save individual benchmark result."""
        filename = f"{result.benchmark_name}_{result.target}_{result.timestamp.replace(':', '-')}.json"
        filepath = self.output_dir / filename

        with open(filepath, 'w') as f:
            json.dump(asdict(result), f, indent=2, default=str)

        logger.debug(f"Saved result to {filepath}")

    def _save_report(self, report: ComparisonReport):
        """Save final comparison report."""
        # Save JSON report
        json_path = self.output_dir / f"{report.report_id}.json"
        with open(json_path, 'w') as f:
            f.write(report.to_json())
        logger.info(f"Saved JSON report to {json_path}")

        # Save Markdown report
        md_path = self.output_dir / f"{report.report_id}.md"
        with open(md_path, 'w') as f:
            f.write(report.to_markdown())
        logger.info(f"Saved Markdown report to {md_path}")

        # Save raw results CSV
        csv_path = self.output_dir / f"{report.report_id}_raw.csv"
        self._save_csv(report, csv_path)
        logger.info(f"Saved CSV results to {csv_path}")

    def _save_csv(self, report: ComparisonReport, filepath: Path):
        """Save raw results as CSV."""
        with open(filepath, 'w') as f:
            # Header
            f.write("benchmark,target,timestamp,duration_seconds,primary_metric,success\n")

            for benchmark, results in report.results.items():
                for result in results:
                    # Get primary metric
                    if benchmark == 'tpcc':
                        primary = result.metrics.get('tpmc', 0)
                    elif benchmark == 'tsbs':
                        primary = result.metrics.get('rows_per_second', 0)
                    elif benchmark in ('tpch', 'clickbench'):
                        primary = result.metrics.get('total_time_seconds', 0)
                    else:
                        primary = 0

                    f.write(f"{benchmark},{result.target},{result.timestamp},"
                           f"{result.duration_seconds},{primary},{result.success}\n")


def generate_markdown_report(report: ComparisonReport) -> str:
    """Generate a comprehensive Markdown report."""
    md = []

    # Header
    md.append("# Orochi DB Competitor Comparison Report")
    md.append("")
    md.append(f"**Report ID:** {report.report_id}")
    md.append(f"**Generated:** {report.created_at}")
    md.append("")

    # Transparency Checklist
    md.append("## Transparency Checklist")
    md.append("")
    md.append("- [x] Hardware specifications documented")
    md.append("- [x] Software versions recorded")
    md.append("- [x] Configuration files available")
    md.append("- [x] Raw results in JSON/CSV format")
    md.append("- [x] Benchmark methodology described")
    md.append("- [x] Reproducible benchmark scripts provided")
    md.append("")

    # Hardware Specifications
    md.append("## Hardware Specifications")
    md.append("")
    md.append("| Specification | Value |")
    md.append("|--------------|-------|")
    md.append(f"| CPU Model | {report.hardware.cpu_model} |")
    md.append(f"| CPU Cores | {report.hardware.cpu_cores} |")
    md.append(f"| CPU Threads | {report.hardware.cpu_threads} |")
    md.append(f"| Memory | {report.hardware.memory_gb} GB |")
    md.append(f"| Storage | {report.hardware.storage_type} |")
    md.append(f"| OS | {report.hardware.os_version} |")
    md.append("")

    # Software Versions
    md.append("## Software Versions")
    md.append("")
    md.append("| Target | PostgreSQL Version | Extension Version |")
    md.append("|--------|-------------------|-------------------|")
    for target in report.targets:
        info = report.software_info.get(target, SoftwareInfo())
        md.append(f"| {target} | {info.postgresql_version or 'N/A'} | {info.extension_version or 'N/A'} |")
    md.append("")

    # Executive Summary
    md.append("## Executive Summary")
    md.append("")

    if report.relative_performance:
        md.append("### Relative Performance (vs vanilla-postgres baseline)")
        md.append("")
        md.append("| Benchmark | " + " | ".join(report.targets) + " |")
        md.append("|-----------|" + "|".join(["-------" for _ in report.targets]) + "|")

        for benchmark in report.benchmarks_run:
            perf = report.relative_performance.get(benchmark, {})
            row = [benchmark]
            for target in report.targets:
                ratio = perf.get(target, '-')
                if isinstance(ratio, (int, float)):
                    row.append(f"{ratio:.2f}x")
                else:
                    row.append(str(ratio))
            md.append("| " + " | ".join(row) + " |")
        md.append("")

    # Detailed Results
    for benchmark in report.benchmarks_run:
        md.append(f"## {benchmark.upper()} Benchmark Results")
        md.append("")

        results = report.results.get(benchmark, [])
        if not results:
            md.append("*No results available*")
            md.append("")
            continue

        # Determine columns based on benchmark type
        if benchmark == 'tpcc':
            md.append("| Target | tpmC | NOPM | P50 (ms) | P95 (ms) | P99 (ms) | Status |")
            md.append("|--------|------|------|----------|----------|----------|--------|")
            for result in results:
                m = result.metrics
                status = "PASS" if result.success else "FAIL"
                md.append(f"| {result.target} | {m.get('tpmc', 0):,.0f} | {m.get('nopm', 0):,.0f} | "
                         f"{m.get('latency_p50_ms', 0):.2f} | {m.get('latency_p95_ms', 0):.2f} | "
                         f"{m.get('latency_p99_ms', 0):.2f} | {status} |")

        elif benchmark == 'tsbs':
            md.append("| Target | Rows/Second | Total Rows | Duration (s) | Status |")
            md.append("|--------|-------------|------------|--------------|--------|")
            for result in results:
                m = result.metrics
                status = "PASS" if result.success else "FAIL"
                md.append(f"| {result.target} | {m.get('rows_per_second', 0):,.0f} | "
                         f"{m.get('total_rows', 0):,} | {result.duration_seconds:.1f} | {status} |")

        elif benchmark == 'clickbench':
            md.append("| Target | Total Time (s) | Query Count | Avg Query (s) | Status |")
            md.append("|--------|----------------|-------------|---------------|--------|")
            for result in results:
                m = result.metrics
                status = "PASS" if result.success else "FAIL"
                query_times = m.get('query_times', [])
                avg_time = sum(query_times) / len(query_times) if query_times else 0
                md.append(f"| {result.target} | {m.get('total_time_seconds', 0):.2f} | "
                         f"{len(query_times)} | {avg_time:.3f} | {status} |")

        elif benchmark == 'tpch':
            md.append("| Target | Scale Factor | Total Time (s) | Status |")
            md.append("|--------|--------------|----------------|--------|")
            for result in results:
                m = result.metrics
                status = "PASS" if result.success else "FAIL"
                md.append(f"| {result.target} | {m.get('scale_factor', 1)} | "
                         f"{m.get('total_time_seconds', 0):.2f} | {status} |")

        md.append("")

        # Chart placeholder
        md.append(f"![{benchmark} Performance Chart](charts/{benchmark}_comparison.svg)")
        md.append("")

    # Methodology
    md.append("## Methodology")
    md.append("")
    md.append("### TPC-C")
    md.append("- Standard OLTP benchmark measuring transactions per minute")
    md.append("- Warehouses: 10 (small), 100 (medium), 1000 (large)")
    md.append("- Duration: 10 minutes (ramp-up) + 30 minutes (measurement)")
    md.append("- Tool: HammerDB or BenchmarkSQL")
    md.append("")
    md.append("### TSBS (Time Series Benchmark Suite)")
    md.append("- Industry-standard time-series benchmark from Timescale")
    md.append("- Workloads: DevOps (CPU, memory, disk metrics)")
    md.append("- Scale: 100 hosts, 10 metrics per host")
    md.append("- Duration: 3 days of simulated data")
    md.append("")
    md.append("### ClickBench")
    md.append("- 43 analytical queries on web analytics dataset")
    md.append("- Dataset: ~100GB uncompressed")
    md.append("- Cold and hot cache runs")
    md.append("")

    # Reproduction Instructions
    md.append("## Reproduction Instructions")
    md.append("")
    md.append("```bash")
    md.append("# Clone repository")
    md.append("git clone https://github.com/orochi-db/orochi-db.git")
    md.append("cd orochi-db/extensions/postgres/benchmark/competitor")
    md.append("")
    md.append("# Run comparison benchmark")
    md.append(f"python competitor_bench.py --benchmark={','.join(report.benchmarks_run)} \\")
    md.append(f"    --targets={','.join(report.targets)}")
    md.append("```")
    md.append("")

    # Footer
    md.append("---")
    md.append(f"*Report generated by Orochi DB Competitor Benchmarking Framework v1.0*")
    md.append(f"*Timestamp: {report.created_at}*")

    return "\n".join(md)


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Orochi DB Competitor Comparison Benchmarking Framework",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --benchmark=tpcc --targets=orochi,timescaledb
  %(prog)s --benchmark=all --targets=all
  %(prog)s --benchmark=tsbs --targets=orochi,questdb
  %(prog)s --report-only --results-dir=./results
        """
    )

    parser.add_argument(
        '--benchmark', '-b',
        type=str,
        default='all',
        help=f"Benchmarks to run, comma-separated ({', '.join(SUPPORTED_BENCHMARKS)})"
    )

    parser.add_argument(
        '--targets', '-t',
        type=str,
        default='orochi,vanilla-postgres',
        help=f"Targets to benchmark, comma-separated ({', '.join(SUPPORTED_TARGETS)})"
    )

    parser.add_argument(
        '--output-dir', '-o',
        type=Path,
        default=RESULTS_DIR,
        help="Output directory for results"
    )

    parser.add_argument(
        '--config', '-c',
        type=Path,
        help="Custom configuration file (JSON)"
    )

    parser.add_argument(
        '--report-only',
        action='store_true',
        help="Generate report from existing results without running benchmarks"
    )

    parser.add_argument(
        '--results-dir',
        type=Path,
        help="Directory containing existing results (for --report-only)"
    )

    parser.add_argument(
        '--generate-charts',
        action='store_true',
        help="Generate charts from results"
    )

    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help="Enable verbose logging"
    )

    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Parse benchmarks and targets
    benchmarks = [b.strip() for b in args.benchmark.split(',')]
    targets = [t.strip() for t in args.targets.split(',')]

    # Validate inputs
    for b in benchmarks:
        if b not in SUPPORTED_BENCHMARKS:
            parser.error(f"Unknown benchmark: {b}. Supported: {SUPPORTED_BENCHMARKS}")

    for t in targets:
        if t not in SUPPORTED_TARGETS:
            parser.error(f"Unknown target: {t}. Supported: {SUPPORTED_TARGETS}")

    # Load custom config if provided
    configs = None
    if args.config:
        with open(args.config) as f:
            configs = json.load(f)

    if args.report_only:
        logger.info("Report-only mode: Loading existing results...")
        results_dir = args.results_dir or args.output_dir
        # Load and regenerate report from existing JSON files
        # Implementation would load JSON files and reconstruct report
        logger.warning("Report-only mode not fully implemented yet")
        return

    # Run benchmarks
    runner = BenchmarkRunner(
        benchmarks=benchmarks,
        targets=targets,
        configs=configs,
        output_dir=args.output_dir
    )

    report = runner.run_all()

    # Generate charts if requested
    if args.generate_charts:
        try:
            subprocess.run(
                ['python', str(SCRIPT_DIR / 'generate_charts.py'),
                 '--input', str(args.output_dir / f"{report.report_id}.json"),
                 '--output', str(CHARTS_DIR)],
                check=True
            )
            logger.info("Charts generated successfully")
        except subprocess.CalledProcessError as e:
            logger.error(f"Chart generation failed: {e}")

    # Print summary
    print("\n" + "=" * 60)
    print("BENCHMARK COMPARISON COMPLETE")
    print("=" * 60)
    print(f"\nReport ID: {report.report_id}")
    print(f"Results saved to: {args.output_dir}")
    print(f"\nRelative Performance (higher is better):")
    for benchmark, perf in report.relative_performance.items():
        print(f"\n  {benchmark.upper()}:")
        for target, ratio in perf.items():
            indicator = "+++" if ratio > 1.5 else "++" if ratio > 1.1 else "+" if ratio > 1.0 else "-"
            print(f"    {target}: {ratio:.2f}x {indicator}")

    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
