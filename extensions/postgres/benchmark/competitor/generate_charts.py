#!/usr/bin/env python3
"""
Chart Generation for Orochi DB Competitor Comparison

Generates SVG and PNG charts from benchmark results for inclusion
in comparison reports.

Features:
- Throughput comparison bar charts
- Latency percentile distributions
- Scalability curves
- Time-series performance graphs
- Export to SVG and PNG formats

Usage:
  python generate_charts.py --input results/comparison_20240101_120000.json
  python generate_charts.py --input-dir results/ --run-id comparison_20240101_120000
  python generate_charts.py --input results/*.json --output charts/
"""

import argparse
import json
import logging
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
import math

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Try to import matplotlib, but provide fallback
try:
    import matplotlib
    matplotlib.use('Agg')  # Non-interactive backend
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    from matplotlib.ticker import MaxNLocator
    MATPLOTLIB_AVAILABLE = True
except ImportError:
    MATPLOTLIB_AVAILABLE = False
    logger.warning("matplotlib not available. Charts will be generated as SVG-only using native code.")

# Color palette (colorblind-friendly)
COLORS = {
    'orochi': '#2E86AB',      # Blue
    'timescaledb': '#A23B72',  # Magenta
    'citus': '#F18F01',        # Orange
    'vanilla-postgres': '#C73E1D',  # Red
    'questdb': '#3B1F2B',      # Dark purple
    'baseline': '#666666'      # Gray
}

# Chart styling
CHART_STYLE = {
    'figure.figsize': (10, 6),
    'font.size': 11,
    'axes.titlesize': 14,
    'axes.labelsize': 12,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'legend.fontsize': 10,
    'figure.dpi': 100
}


@dataclass
class ChartData:
    """Container for chart data."""
    title: str
    xlabel: str
    ylabel: str
    labels: List[str]
    values: Dict[str, List[float]]
    chart_type: str = 'bar'
    subtitle: str = ''


def load_results(input_path: Path) -> Dict[str, Any]:
    """Load benchmark results from JSON file."""
    with open(input_path) as f:
        return json.load(f)


def extract_tpcc_data(results: Dict[str, Any]) -> ChartData:
    """Extract TPC-C data for charting."""
    tpcc_results = results.get('results', {}).get('tpcc', [])

    if not tpcc_results:
        return None

    labels = []
    tpmc_values = []
    nopm_values = []

    for result in tpcc_results:
        if result.get('success'):
            labels.append(result['target'])
            tpmc_values.append(result['metrics'].get('tpmc', 0))
            nopm_values.append(result['metrics'].get('nopm', 0))

    return ChartData(
        title='TPC-C Throughput Comparison',
        xlabel='Database',
        ylabel='Transactions per Minute',
        labels=labels,
        values={'tpmC': tpmc_values, 'NOPM': nopm_values},
        chart_type='grouped_bar'
    )


def extract_tpcc_latency_data(results: Dict[str, Any]) -> ChartData:
    """Extract TPC-C latency data for charting."""
    tpcc_results = results.get('results', {}).get('tpcc', [])

    if not tpcc_results:
        return None

    labels = []
    p50_values = []
    p95_values = []
    p99_values = []

    for result in tpcc_results:
        if result.get('success'):
            labels.append(result['target'])
            p50_values.append(result['metrics'].get('latency_p50_ms', 0))
            p95_values.append(result['metrics'].get('latency_p95_ms', 0))
            p99_values.append(result['metrics'].get('latency_p99_ms', 0))

    return ChartData(
        title='TPC-C Latency Distribution',
        xlabel='Database',
        ylabel='Latency (ms)',
        labels=labels,
        values={'P50': p50_values, 'P95': p95_values, 'P99': p99_values},
        chart_type='grouped_bar'
    )


def extract_tsbs_data(results: Dict[str, Any]) -> ChartData:
    """Extract TSBS data for charting."""
    tsbs_results = results.get('results', {}).get('tsbs', [])

    if not tsbs_results:
        return None

    labels = []
    rps_values = []

    for result in tsbs_results:
        if result.get('success'):
            labels.append(result['target'])
            rps_values.append(result['metrics'].get('rows_per_second', 0))

    return ChartData(
        title='TSBS Ingestion Rate',
        xlabel='Database',
        ylabel='Rows per Second',
        labels=labels,
        values={'Ingestion Rate': rps_values},
        chart_type='bar'
    )


def extract_clickbench_data(results: Dict[str, Any]) -> ChartData:
    """Extract ClickBench data for charting."""
    clickbench_results = results.get('results', {}).get('clickbench', [])

    if not clickbench_results:
        return None

    labels = []
    total_times = []

    for result in clickbench_results:
        if result.get('success'):
            labels.append(result['target'])
            total_times.append(result['metrics'].get('total_time_seconds', 0))

    return ChartData(
        title='ClickBench Total Query Time',
        xlabel='Database',
        ylabel='Total Time (seconds)',
        labels=labels,
        values={'Total Time': total_times},
        chart_type='bar'
    )


def extract_relative_performance_data(results: Dict[str, Any]) -> ChartData:
    """Extract relative performance data for charting."""
    rel_perf = results.get('relative_performance', {})

    if not rel_perf:
        return None

    benchmarks = list(rel_perf.keys())
    targets = set()
    for bench_data in rel_perf.values():
        targets.update(bench_data.keys())
    targets = sorted(list(targets))

    values = {target: [] for target in targets}

    for benchmark in benchmarks:
        bench_data = rel_perf.get(benchmark, {})
        for target in targets:
            values[target].append(bench_data.get(target, 0))

    return ChartData(
        title='Relative Performance vs Baseline',
        xlabel='Benchmark',
        ylabel='Performance Ratio (higher is better)',
        labels=benchmarks,
        values=values,
        chart_type='grouped_bar',
        subtitle='Baseline = 1.0x'
    )


def generate_bar_chart(data: ChartData, output_path: Path, format: str = 'svg'):
    """Generate a bar chart."""
    if not MATPLOTLIB_AVAILABLE:
        generate_svg_bar_chart(data, output_path)
        return

    plt.style.use('seaborn-v0_8-whitegrid')

    fig, ax = plt.subplots(figsize=CHART_STYLE['figure.figsize'])

    x = range(len(data.labels))

    for i, (series_name, values) in enumerate(data.values.items()):
        color = COLORS.get(series_name.lower(), COLORS.get('baseline'))
        bars = ax.bar(
            [xi + i * 0.8 / len(data.values) for xi in x],
            values,
            width=0.8 / len(data.values),
            label=series_name,
            color=color,
            edgecolor='white',
            linewidth=0.5
        )

        # Add value labels on bars
        for bar, val in zip(bars, values):
            height = bar.get_height()
            ax.annotate(
                f'{val:,.0f}' if val > 100 else f'{val:.2f}',
                xy=(bar.get_x() + bar.get_width() / 2, height),
                xytext=(0, 3),
                textcoords="offset points",
                ha='center', va='bottom',
                fontsize=8
            )

    ax.set_xlabel(data.xlabel)
    ax.set_ylabel(data.ylabel)
    ax.set_title(data.title, fontweight='bold')

    if data.subtitle:
        ax.text(
            0.5, 0.98, data.subtitle,
            transform=ax.transAxes,
            ha='center', va='top',
            fontsize=9, style='italic', color='gray'
        )

    ax.set_xticks([xi + 0.4 for xi in x])
    ax.set_xticklabels(data.labels)
    ax.legend(loc='upper right')
    ax.yaxis.set_major_locator(MaxNLocator(integer=True, nbins=8))

    plt.tight_layout()

    # Save in requested format
    if format == 'both':
        plt.savefig(output_path.with_suffix('.svg'), format='svg', bbox_inches='tight')
        plt.savefig(output_path.with_suffix('.png'), format='png', dpi=150, bbox_inches='tight')
    else:
        plt.savefig(output_path.with_suffix(f'.{format}'), format=format,
                   dpi=150 if format == 'png' else None, bbox_inches='tight')

    plt.close()
    logger.info(f"Generated chart: {output_path}")


def generate_grouped_bar_chart(data: ChartData, output_path: Path, format: str = 'svg'):
    """Generate a grouped bar chart."""
    if not MATPLOTLIB_AVAILABLE:
        generate_svg_grouped_bar_chart(data, output_path)
        return

    plt.style.use('seaborn-v0_8-whitegrid')

    fig, ax = plt.subplots(figsize=CHART_STYLE['figure.figsize'])

    x = range(len(data.labels))
    n_series = len(data.values)
    width = 0.8 / n_series

    for i, (series_name, values) in enumerate(data.values.items()):
        color = COLORS.get(series_name.lower(), list(COLORS.values())[i % len(COLORS)])
        offset = (i - n_series / 2 + 0.5) * width

        bars = ax.bar(
            [xi + offset for xi in x],
            values,
            width=width,
            label=series_name,
            color=color,
            edgecolor='white',
            linewidth=0.5
        )

    ax.set_xlabel(data.xlabel)
    ax.set_ylabel(data.ylabel)
    ax.set_title(data.title, fontweight='bold')

    if data.subtitle:
        ax.text(
            0.5, 0.98, data.subtitle,
            transform=ax.transAxes,
            ha='center', va='top',
            fontsize=9, style='italic', color='gray'
        )

    ax.set_xticks(x)
    ax.set_xticklabels(data.labels)
    ax.legend(loc='upper right')

    # Add reference line at 1.0 for relative performance charts
    if 'ratio' in data.ylabel.lower() or 'relative' in data.title.lower():
        ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=1, alpha=0.7)

    plt.tight_layout()

    # Save in requested format
    if format == 'both':
        plt.savefig(output_path.with_suffix('.svg'), format='svg', bbox_inches='tight')
        plt.savefig(output_path.with_suffix('.png'), format='png', dpi=150, bbox_inches='tight')
    else:
        plt.savefig(output_path.with_suffix(f'.{format}'), format=format,
                   dpi=150 if format == 'png' else None, bbox_inches='tight')

    plt.close()
    logger.info(f"Generated chart: {output_path}")


def generate_svg_bar_chart(data: ChartData, output_path: Path):
    """Generate SVG bar chart without matplotlib."""
    width = 800
    height = 500
    margin = {'top': 60, 'right': 80, 'bottom': 80, 'left': 80}

    chart_width = width - margin['left'] - margin['right']
    chart_height = height - margin['top'] - margin['bottom']

    # Get all values and find max
    all_values = []
    for values in data.values.values():
        all_values.extend(values)
    max_val = max(all_values) if all_values else 1
    max_val = max_val * 1.1  # Add 10% padding

    n_bars = len(data.labels)
    n_series = len(data.values)
    bar_group_width = chart_width / n_bars
    bar_width = bar_group_width * 0.8 / n_series

    # Generate SVG
    svg_parts = [
        f'<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}">',
        f'  <style>',
        f'    .title {{ font-family: sans-serif; font-size: 16px; font-weight: bold; }}',
        f'    .axis-label {{ font-family: sans-serif; font-size: 12px; }}',
        f'    .tick-label {{ font-family: sans-serif; font-size: 10px; }}',
        f'    .legend-text {{ font-family: sans-serif; font-size: 10px; }}',
        f'    .value-label {{ font-family: sans-serif; font-size: 8px; }}',
        f'  </style>',
        f'',
        f'  <!-- Background -->',
        f'  <rect width="{width}" height="{height}" fill="white"/>',
        f'',
        f'  <!-- Title -->',
        f'  <text x="{width/2}" y="30" text-anchor="middle" class="title">{data.title}</text>',
        f'',
        f'  <!-- Chart area -->',
        f'  <g transform="translate({margin["left"]}, {margin["top"]})">'
    ]

    # Grid lines
    n_gridlines = 5
    for i in range(n_gridlines + 1):
        y = chart_height * (1 - i / n_gridlines)
        val = max_val * i / n_gridlines
        svg_parts.append(f'    <line x1="0" y1="{y}" x2="{chart_width}" y2="{y}" stroke="#ddd" stroke-width="1"/>')
        svg_parts.append(f'    <text x="-10" y="{y + 4}" text-anchor="end" class="tick-label">{val:,.0f}</text>')

    # Bars
    color_list = list(COLORS.values())
    for i, (series_name, values) in enumerate(data.values.items()):
        color = color_list[i % len(color_list)]

        for j, (label, value) in enumerate(zip(data.labels, values)):
            x = j * bar_group_width + (bar_group_width - bar_width * n_series) / 2 + i * bar_width
            bar_height = (value / max_val) * chart_height if max_val > 0 else 0
            y = chart_height - bar_height

            svg_parts.append(f'    <rect x="{x}" y="{y}" width="{bar_width - 2}" height="{bar_height}" fill="{color}"/>')

            # Value label
            val_str = f'{value:,.0f}' if value > 100 else f'{value:.2f}'
            svg_parts.append(f'    <text x="{x + bar_width/2 - 1}" y="{y - 5}" text-anchor="middle" class="value-label">{val_str}</text>')

    # X-axis labels
    for i, label in enumerate(data.labels):
        x = i * bar_group_width + bar_group_width / 2
        svg_parts.append(f'    <text x="{x}" y="{chart_height + 20}" text-anchor="middle" class="tick-label">{label}</text>')

    # Axes
    svg_parts.append(f'    <line x1="0" y1="{chart_height}" x2="{chart_width}" y2="{chart_height}" stroke="black" stroke-width="1"/>')
    svg_parts.append(f'    <line x1="0" y1="0" x2="0" y2="{chart_height}" stroke="black" stroke-width="1"/>')

    # Axis labels
    svg_parts.append(f'    <text x="{chart_width/2}" y="{chart_height + 50}" text-anchor="middle" class="axis-label">{data.xlabel}</text>')
    svg_parts.append(f'    <text x="-50" y="{chart_height/2}" text-anchor="middle" transform="rotate(-90, -50, {chart_height/2})" class="axis-label">{data.ylabel}</text>')

    svg_parts.append('  </g>')

    # Legend
    legend_x = width - margin['right'] + 10
    svg_parts.append(f'  <g transform="translate({legend_x}, {margin["top"]})">')
    for i, series_name in enumerate(data.values.keys()):
        color = color_list[i % len(color_list)]
        y = i * 20
        svg_parts.append(f'    <rect x="0" y="{y}" width="12" height="12" fill="{color}"/>')
        svg_parts.append(f'    <text x="16" y="{y + 10}" class="legend-text">{series_name}</text>')
    svg_parts.append('  </g>')

    svg_parts.append('</svg>')

    # Write SVG
    svg_content = '\n'.join(svg_parts)
    output_path.with_suffix('.svg').write_text(svg_content)
    logger.info(f"Generated SVG chart: {output_path.with_suffix('.svg')}")


def generate_svg_grouped_bar_chart(data: ChartData, output_path: Path):
    """Generate SVG grouped bar chart without matplotlib."""
    # Use the same implementation as bar chart for simplicity
    generate_svg_bar_chart(data, output_path)


def generate_all_charts(results: Dict[str, Any], output_dir: Path, format: str = 'svg'):
    """Generate all charts from results."""
    output_dir.mkdir(parents=True, exist_ok=True)

    charts_generated = []

    # TPC-C throughput
    tpcc_data = extract_tpcc_data(results)
    if tpcc_data:
        output_path = output_dir / 'tpcc_throughput'
        if tpcc_data.chart_type == 'grouped_bar':
            generate_grouped_bar_chart(tpcc_data, output_path, format)
        else:
            generate_bar_chart(tpcc_data, output_path, format)
        charts_generated.append(output_path)

    # TPC-C latency
    tpcc_latency_data = extract_tpcc_latency_data(results)
    if tpcc_latency_data:
        output_path = output_dir / 'tpcc_latency'
        generate_grouped_bar_chart(tpcc_latency_data, output_path, format)
        charts_generated.append(output_path)

    # TSBS ingestion
    tsbs_data = extract_tsbs_data(results)
    if tsbs_data:
        output_path = output_dir / 'tsbs_ingestion'
        generate_bar_chart(tsbs_data, output_path, format)
        charts_generated.append(output_path)

    # ClickBench
    clickbench_data = extract_clickbench_data(results)
    if clickbench_data:
        output_path = output_dir / 'clickbench_total'
        generate_bar_chart(clickbench_data, output_path, format)
        charts_generated.append(output_path)

    # Relative performance
    rel_data = extract_relative_performance_data(results)
    if rel_data:
        output_path = output_dir / 'relative_performance'
        generate_grouped_bar_chart(rel_data, output_path, format)
        charts_generated.append(output_path)

    return charts_generated


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Generate charts from Orochi DB benchmark results"
    )

    parser.add_argument(
        '--input', '-i',
        type=Path,
        help="Input JSON results file"
    )

    parser.add_argument(
        '--input-dir',
        type=Path,
        help="Directory containing JSON results files"
    )

    parser.add_argument(
        '--output', '-o',
        type=Path,
        default=Path('charts'),
        help="Output directory for charts"
    )

    parser.add_argument(
        '--run-id',
        type=str,
        help="Filter to specific run ID"
    )

    parser.add_argument(
        '--format', '-f',
        choices=['svg', 'png', 'both'],
        default='svg',
        help="Output format (default: svg)"
    )

    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help="Enable verbose logging"
    )

    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Find input files
    input_files = []

    if args.input:
        input_files.append(args.input)
    elif args.input_dir:
        pattern = f"{args.run_id}*.json" if args.run_id else "*.json"
        input_files = list(args.input_dir.glob(pattern))
    else:
        parser.error("Either --input or --input-dir must be specified")

    if not input_files:
        logger.error("No input files found")
        return 1

    # Process each input file
    all_charts = []

    for input_file in input_files:
        logger.info(f"Processing: {input_file}")

        try:
            results = load_results(input_file)
            charts = generate_all_charts(results, args.output, args.format)
            all_charts.extend(charts)
        except Exception as e:
            logger.error(f"Failed to process {input_file}: {e}")
            continue

    # Summary
    logger.info(f"\n{'='*50}")
    logger.info(f"Chart Generation Complete")
    logger.info(f"{'='*50}")
    logger.info(f"Charts generated: {len(all_charts)}")
    logger.info(f"Output directory: {args.output}")

    for chart in all_charts:
        logger.info(f"  - {chart}")

    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
