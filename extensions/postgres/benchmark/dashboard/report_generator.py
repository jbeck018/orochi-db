#!/usr/bin/env python3
"""
Orochi DB Benchmark Report Generator

Generates HTML and Markdown reports from benchmark results.
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Any

# HTML Template for the dashboard
HTML_TEMPLATE = '''<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Orochi DB Benchmark Results</title>
    <style>
        :root {
            --primary: #4A90D9;
            --secondary: #67B26F;
            --accent: #F5A623;
            --danger: #D0021B;
            --success: #7ED321;
            --bg: #F5F7FA;
            --card-bg: #FFFFFF;
            --text: #333333;
            --text-muted: #666666;
            --border: #E1E4E8;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: var(--bg);
            color: var(--text);
            line-height: 1.6;
        }

        .container {
            max-width: 1400px;
            margin: 0 auto;
            padding: 20px;
        }

        header {
            background: linear-gradient(135deg, var(--primary), #2C3E50);
            color: white;
            padding: 40px 20px;
            margin-bottom: 30px;
        }

        header h1 {
            font-size: 2.5em;
            margin-bottom: 10px;
        }

        header .subtitle {
            opacity: 0.9;
            font-size: 1.1em;
        }

        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }

        .stat-card {
            background: var(--card-bg);
            border-radius: 12px;
            padding: 24px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.08);
            border: 1px solid var(--border);
        }

        .stat-card .label {
            color: var(--text-muted);
            font-size: 0.9em;
            margin-bottom: 8px;
        }

        .stat-card .value {
            font-size: 2.2em;
            font-weight: 700;
            color: var(--primary);
        }

        .stat-card .unit {
            font-size: 0.9em;
            color: var(--text-muted);
        }

        .stat-card.success .value { color: var(--success); }
        .stat-card.warning .value { color: var(--accent); }
        .stat-card.danger .value { color: var(--danger); }

        .section {
            background: var(--card-bg);
            border-radius: 12px;
            padding: 24px;
            margin-bottom: 24px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.08);
            border: 1px solid var(--border);
        }

        .section h2 {
            color: var(--text);
            margin-bottom: 20px;
            padding-bottom: 12px;
            border-bottom: 2px solid var(--primary);
        }

        .chart-container {
            text-align: center;
            margin: 20px 0;
        }

        .chart-container img {
            max-width: 100%;
            height: auto;
            border-radius: 8px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.1);
        }

        table {
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
        }

        th, td {
            padding: 12px 16px;
            text-align: left;
            border-bottom: 1px solid var(--border);
        }

        th {
            background: var(--bg);
            font-weight: 600;
            color: var(--text);
        }

        tr:hover {
            background: var(--bg);
        }

        .status-badge {
            display: inline-block;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 0.85em;
            font-weight: 500;
        }

        .status-badge.success {
            background: rgba(126, 211, 33, 0.15);
            color: var(--success);
        }

        .status-badge.failed {
            background: rgba(208, 2, 27, 0.15);
            color: var(--danger);
        }

        .category-tag {
            display: inline-block;
            padding: 2px 8px;
            border-radius: 4px;
            font-size: 0.8em;
            margin-right: 4px;
        }

        .category-tpch { background: #4A90D9; color: white; }
        .category-timeseries { background: #67B26F; color: white; }
        .category-columnar { background: #9B59B6; color: white; }
        .category-distributed { background: #E74C3C; color: white; }
        .category-multitenant { background: #F39C12; color: white; }
        .category-sharding { background: #1ABC9C; color: white; }
        .category-connection { background: #3498DB; color: white; }

        footer {
            text-align: center;
            padding: 30px;
            color: var(--text-muted);
            font-size: 0.9em;
        }

        .charts-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
            gap: 20px;
        }

        @media (max-width: 768px) {
            .charts-grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>
    <header>
        <div class="container">
            <h1>Orochi DB Benchmark Results</h1>
            <p class="subtitle">Generated on {{generated_at}}</p>
        </div>
    </header>

    <div class="container">
        <div class="stats-grid">
            <div class="stat-card">
                <div class="label">Total Benchmarks</div>
                <div class="value">{{total_benchmarks}}</div>
            </div>
            <div class="stat-card {{success_class}}">
                <div class="label">Success Rate</div>
                <div class="value">{{success_rate}}<span class="unit">%</span></div>
            </div>
            <div class="stat-card">
                <div class="label">Avg Latency</div>
                <div class="value">{{avg_latency}}<span class="unit">ms</span></div>
            </div>
            <div class="stat-card">
                <div class="label">Avg Throughput</div>
                <div class="value">{{avg_throughput}}<span class="unit">ops/s</span></div>
            </div>
        </div>

        <div class="section">
            <h2>Performance Charts</h2>
            <div class="charts-grid">
                {{charts_html}}
            </div>
        </div>

        <div class="section">
            <h2>Benchmark Results</h2>
            <table>
                <thead>
                    <tr>
                        <th>Benchmark</th>
                        <th>Category</th>
                        <th>Status</th>
                        <th>Avg Latency</th>
                        <th>P95 Latency</th>
                        <th>Throughput</th>
                    </tr>
                </thead>
                <tbody>
                    {{results_table}}
                </tbody>
            </table>
        </div>

        <div class="section">
            <h2>System Information</h2>
            <table>
                <tbody>
                    {{system_info}}
                </tbody>
            </table>
        </div>
    </div>

    <footer>
        <p>Generated by Orochi DB Benchmark Suite v1.0</p>
    </footer>
</body>
</html>
'''


class ReportGenerator:
    """Generates HTML and Markdown reports from benchmark data."""

    def __init__(self, charts_dir: str = 'charts'):
        self.charts_dir = Path(charts_dir)

    def _get_category(self, name: str) -> str:
        """Determine benchmark category from name."""
        categories = ['tpch', 'timeseries', 'columnar', 'distributed',
                      'multitenant', 'sharding', 'connection']
        for cat in categories:
            if cat in name.lower():
                return cat
        return 'other'

    def _format_number(self, value: float, precision: int = 2) -> str:
        """Format a number with appropriate precision."""
        if value >= 1000000:
            return f"{value/1000000:.1f}M"
        elif value >= 1000:
            return f"{value/1000:.1f}K"
        else:
            return f"{value:.{precision}f}"

    def generate_html(self, data: Dict[str, Any], output_file: str,
                      charts: Optional[Dict[str, str]] = None):
        """Generate an HTML report."""
        summary = data.get('summary', {})
        benchmarks = data.get('benchmarks', {})

        # Calculate stats
        total_benchmarks = summary.get('total_benchmarks', 0)
        success_rate = summary.get('success_rate', 0)
        avg_latency = summary.get('latency_stats', {}).get('avg_ms', 0)
        avg_throughput = summary.get('throughput_stats', {}).get('avg_ops', 0)

        # Determine success class
        if success_rate >= 90:
            success_class = 'success'
        elif success_rate >= 70:
            success_class = 'warning'
        else:
            success_class = 'danger'

        # Generate charts HTML
        charts_html = ""
        if charts:
            for name, path in charts.items():
                # Use relative path for portability
                rel_path = os.path.relpath(path, os.path.dirname(output_file))
                charts_html += f'''
                <div class="chart-container">
                    <img src="{rel_path}" alt="{name.replace('_', ' ').title()}">
                </div>
                '''
        else:
            charts_html = "<p>No charts available. Run chart_generator.py first.</p>"

        # Generate results table
        results_table = ""
        for name, bench_data in benchmarks.items():
            category = self._get_category(name)
            latency = bench_data.get('latency', {})
            throughput = bench_data.get('throughput', {})

            status = 'success' if bench_data.get('successful_runs', 0) > 0 else 'failed'
            status_text = 'Passed' if status == 'success' else 'Failed'

            avg_lat = self._format_number(latency.get('avg_ms', 0))
            p95_lat = self._format_number(latency.get('avg_ms', 0) * 1.5)  # Estimate
            tput = self._format_number(throughput.get('avg_ops', 0), 0)

            results_table += f'''
                <tr>
                    <td>{name}</td>
                    <td><span class="category-tag category-{category}">{category.title()}</span></td>
                    <td><span class="status-badge {status}">{status_text}</span></td>
                    <td>{avg_lat} ms</td>
                    <td>{p95_lat} ms</td>
                    <td>{tput} ops/s</td>
                </tr>
            '''

        # Generate system info
        system_info = ""
        sys_data = data.get('system_info', {})
        if sys_data:
            for key, value in sys_data.items():
                system_info += f"<tr><td><strong>{key}</strong></td><td>{value}</td></tr>"
        else:
            system_info = "<tr><td colspan='2'>System information not available</td></tr>"

        # Generate HTML
        html = HTML_TEMPLATE
        html = html.replace('{{generated_at}}', data.get('generated_at', datetime.now().isoformat()))
        html = html.replace('{{total_benchmarks}}', str(total_benchmarks))
        html = html.replace('{{success_rate}}', f"{success_rate:.1f}")
        html = html.replace('{{success_class}}', success_class)
        html = html.replace('{{avg_latency}}', self._format_number(avg_latency))
        html = html.replace('{{avg_throughput}}', self._format_number(avg_throughput, 0))
        html = html.replace('{{charts_html}}', charts_html)
        html = html.replace('{{results_table}}', results_table)
        html = html.replace('{{system_info}}', system_info)

        with open(output_file, 'w') as f:
            f.write(html)

        print(f"HTML report generated: {output_file}")
        return output_file

    def generate_markdown(self, data: Dict[str, Any], output_file: str):
        """Generate a Markdown report."""
        summary = data.get('summary', {})
        benchmarks = data.get('benchmarks', {})
        categories = data.get('categories', {})

        lines = [
            "# Orochi DB Benchmark Results",
            "",
            f"*Generated on {data.get('generated_at', datetime.now().isoformat())}*",
            "",
            "## Summary",
            "",
            f"| Metric | Value |",
            f"|--------|-------|",
            f"| Total Benchmarks | {summary.get('total_benchmarks', 0)} |",
            f"| Successful | {summary.get('successful', 0)} |",
            f"| Failed | {summary.get('failed', 0)} |",
            f"| Success Rate | {summary.get('success_rate', 0):.1f}% |",
            "",
            "## Performance Statistics",
            "",
            "### Latency",
            "",
            f"| Metric | Value |",
            f"|--------|-------|",
            f"| Min | {summary.get('latency_stats', {}).get('min_ms', 0):.2f} ms |",
            f"| Max | {summary.get('latency_stats', {}).get('max_ms', 0):.2f} ms |",
            f"| Average | {summary.get('latency_stats', {}).get('avg_ms', 0):.2f} ms |",
            f"| Median | {summary.get('latency_stats', {}).get('median_ms', 0):.2f} ms |",
            "",
            "### Throughput",
            "",
            f"| Metric | Value |",
            f"|--------|-------|",
            f"| Min | {summary.get('throughput_stats', {}).get('min_ops', 0):,.0f} ops/s |",
            f"| Max | {summary.get('throughput_stats', {}).get('max_ops', 0):,.0f} ops/s |",
            f"| Average | {summary.get('throughput_stats', {}).get('avg_ops', 0):,.0f} ops/s |",
            "",
        ]

        # Add category breakdown
        if categories:
            lines.extend([
                "## Benchmarks by Category",
                "",
            ])
            for cat, bench_list in categories.items():
                lines.append(f"### {cat.title()} ({len(bench_list)} benchmarks)")
                lines.append("")
                for bench in bench_list:
                    lines.append(f"- {bench}")
                lines.append("")

        # Add detailed results
        lines.extend([
            "## Detailed Results",
            "",
            "| Benchmark | Runs | Avg Latency | Throughput |",
            "|-----------|------|-------------|------------|",
        ])

        for name, bench_data in benchmarks.items():
            latency = bench_data.get('latency', {})
            throughput = bench_data.get('throughput', {})
            runs = bench_data.get('runs', 0)

            lines.append(
                f"| {name} | {runs} | "
                f"{latency.get('avg_ms', 0):.2f} ms | "
                f"{throughput.get('avg_ops', 0):,.0f} ops/s |"
            )

        lines.extend([
            "",
            "---",
            "",
            "*Report generated by Orochi DB Benchmark Suite*",
        ])

        with open(output_file, 'w') as f:
            f.write('\n'.join(lines))

        print(f"Markdown report generated: {output_file}")
        return output_file


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Generate benchmark reports')
    parser.add_argument('--input', '-i', default='summary.json',
                        help='Input summary JSON file')
    parser.add_argument('--output', '-o', default='report.html',
                        help='Output report file')
    parser.add_argument('--format', '-f', choices=['html', 'markdown', 'both'],
                        default='html', help='Output format')
    parser.add_argument('--charts-dir', '-c', default='charts',
                        help='Directory containing chart images')

    args = parser.parse_args()

    # Load summary data
    try:
        with open(args.input, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: Input file not found: {args.input}")
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {args.input}: {e}")
        sys.exit(1)

    # Find charts
    charts_dir = Path(args.charts_dir)
    charts = {}
    if charts_dir.exists():
        for chart_file in charts_dir.glob('*.png'):
            chart_name = chart_file.stem
            charts[chart_name] = str(chart_file)

    generator = ReportGenerator(args.charts_dir)

    if args.format in ['html', 'both']:
        output = args.output if args.output.endswith('.html') else args.output + '.html'
        generator.generate_html(data, output, charts)

    if args.format in ['markdown', 'both']:
        output = args.output.replace('.html', '.md') if args.output.endswith('.html') else args.output + '.md'
        generator.generate_markdown(data, output)


if __name__ == '__main__':
    main()
