#!/usr/bin/env python3
"""
Orochi DB Benchmark Dashboard Web Server

A simple Flask-based web server for viewing benchmark results.
Provides real-time updates and interactive charts.
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, Any, Optional

# Check for Flask
try:
    from flask import Flask, render_template_string, jsonify, send_from_directory
    from flask_cors import CORS
    HAS_FLASK = True
except ImportError:
    HAS_FLASK = False
    print("Warning: Flask not installed. Install with: pip install flask flask-cors", file=sys.stderr)

# Import local modules
from results_aggregator import ResultsAggregator

# HTML Template for the dashboard
DASHBOARD_TEMPLATE = '''
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Orochi DB Benchmark Dashboard</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {
            --primary: #4A90D9;
            --secondary: #67B26F;
            --accent: #F5A623;
            --danger: #D0021B;
            --bg-dark: #1a1a2e;
            --bg-card: #16213e;
            --text-primary: #ffffff;
            --text-secondary: #a0a0a0;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: var(--bg-dark);
            color: var(--text-primary);
            min-height: 100vh;
        }

        .header {
            background: linear-gradient(135deg, var(--primary), #2C5282);
            padding: 2rem;
            text-align: center;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3);
        }

        .header h1 {
            font-size: 2.5rem;
            margin-bottom: 0.5rem;
        }

        .header p {
            color: rgba(255, 255, 255, 0.8);
        }

        .container {
            max-width: 1400px;
            margin: 0 auto;
            padding: 2rem;
        }

        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 1.5rem;
            margin-bottom: 2rem;
        }

        .stat-card {
            background: var(--bg-card);
            border-radius: 12px;
            padding: 1.5rem;
            text-align: center;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.2);
            transition: transform 0.2s;
        }

        .stat-card:hover {
            transform: translateY(-5px);
        }

        .stat-value {
            font-size: 2.5rem;
            font-weight: bold;
            color: var(--primary);
        }

        .stat-label {
            color: var(--text-secondary);
            margin-top: 0.5rem;
        }

        .stat-card.success .stat-value { color: var(--secondary); }
        .stat-card.warning .stat-value { color: var(--accent); }
        .stat-card.danger .stat-value { color: var(--danger); }

        .charts-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
            gap: 2rem;
            margin-bottom: 2rem;
        }

        .chart-card {
            background: var(--bg-card);
            border-radius: 12px;
            padding: 1.5rem;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.2);
        }

        .chart-card h3 {
            margin-bottom: 1rem;
            color: var(--text-primary);
        }

        .chart-container {
            position: relative;
            height: 300px;
        }

        .results-table {
            width: 100%;
            border-collapse: collapse;
            background: var(--bg-card);
            border-radius: 12px;
            overflow: hidden;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.2);
        }

        .results-table th,
        .results-table td {
            padding: 1rem;
            text-align: left;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }

        .results-table th {
            background: rgba(74, 144, 217, 0.2);
            font-weight: 600;
        }

        .results-table tr:hover {
            background: rgba(255, 255, 255, 0.05);
        }

        .status-badge {
            display: inline-block;
            padding: 0.25rem 0.75rem;
            border-radius: 9999px;
            font-size: 0.875rem;
            font-weight: 500;
        }

        .status-badge.success {
            background: rgba(103, 178, 111, 0.2);
            color: var(--secondary);
        }

        .status-badge.failed {
            background: rgba(208, 2, 27, 0.2);
            color: var(--danger);
        }

        .refresh-btn {
            position: fixed;
            bottom: 2rem;
            right: 2rem;
            background: var(--primary);
            color: white;
            border: none;
            padding: 1rem 1.5rem;
            border-radius: 50px;
            cursor: pointer;
            font-size: 1rem;
            box-shadow: 0 4px 12px rgba(74, 144, 217, 0.4);
            transition: transform 0.2s, box-shadow 0.2s;
        }

        .refresh-btn:hover {
            transform: scale(1.05);
            box-shadow: 0 6px 16px rgba(74, 144, 217, 0.5);
        }

        .last-updated {
            text-align: center;
            color: var(--text-secondary);
            margin-top: 2rem;
            font-size: 0.875rem;
        }

        @media (max-width: 768px) {
            .charts-grid {
                grid-template-columns: 1fr;
            }

            .header h1 {
                font-size: 1.75rem;
            }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>Orochi DB Benchmark Dashboard</h1>
        <p>Real-time performance metrics and analysis</p>
    </div>

    <div class="container">
        <div class="stats-grid" id="statsGrid">
            <!-- Stats will be populated by JavaScript -->
        </div>

        <div class="charts-grid">
            <div class="chart-card">
                <h3>Throughput Comparison</h3>
                <div class="chart-container">
                    <canvas id="throughputChart"></canvas>
                </div>
            </div>
            <div class="chart-card">
                <h3>Latency Distribution</h3>
                <div class="chart-container">
                    <canvas id="latencyChart"></canvas>
                </div>
            </div>
        </div>

        <div class="chart-card">
            <h3>Benchmark Results</h3>
            <table class="results-table" id="resultsTable">
                <thead>
                    <tr>
                        <th>Benchmark</th>
                        <th>Status</th>
                        <th>Queries</th>
                        <th>Avg Latency</th>
                        <th>Throughput</th>
                    </tr>
                </thead>
                <tbody>
                    <!-- Results will be populated by JavaScript -->
                </tbody>
            </table>
        </div>

        <p class="last-updated" id="lastUpdated">Loading...</p>
    </div>

    <button class="refresh-btn" onclick="fetchData()">Refresh</button>

    <script>
        let throughputChart, latencyChart;

        async function fetchData() {
            try {
                const response = await fetch('/api/summary');
                const data = await response.json();
                updateDashboard(data);
            } catch (error) {
                console.error('Error fetching data:', error);
            }
        }

        function updateDashboard(data) {
            const summary = data.summary || {};
            const benchmarks = data.benchmarks || {};

            // Update stats
            const statsHtml = `
                <div class="stat-card">
                    <div class="stat-value">${summary.total_benchmarks || 0}</div>
                    <div class="stat-label">Total Benchmarks</div>
                </div>
                <div class="stat-card success">
                    <div class="stat-value">${(summary.success_rate || 0).toFixed(1)}%</div>
                    <div class="stat-label">Success Rate</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value">${(summary.latency_stats?.avg_ms || 0).toFixed(2)}</div>
                    <div class="stat-label">Avg Latency (ms)</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value">${formatNumber(summary.throughput_stats?.avg_ops || 0)}</div>
                    <div class="stat-label">Avg Throughput (ops/s)</div>
                </div>
            `;
            document.getElementById('statsGrid').innerHTML = statsHtml;

            // Update charts
            updateCharts(benchmarks);

            // Update table
            updateTable(benchmarks);

            // Update timestamp
            document.getElementById('lastUpdated').textContent =
                `Last updated: ${new Date().toLocaleString()}`;
        }

        function updateCharts(benchmarks) {
            const labels = [];
            const throughputs = [];
            const latencies = [];

            for (const [name, bench] of Object.entries(benchmarks)) {
                if (bench.throughput?.avg_ops > 0) {
                    labels.push(name.substring(0, 20));
                    throughputs.push(bench.throughput.avg_ops);
                    latencies.push(bench.latency?.avg_ms || 0);
                }
            }

            // Throughput chart
            if (throughputChart) {
                throughputChart.destroy();
            }

            throughputChart = new Chart(document.getElementById('throughputChart'), {
                type: 'bar',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Operations/sec',
                        data: throughputs,
                        backgroundColor: 'rgba(74, 144, 217, 0.8)',
                        borderColor: 'rgba(74, 144, 217, 1)',
                        borderWidth: 1
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            labels: { color: '#fff' }
                        }
                    },
                    scales: {
                        x: {
                            ticks: { color: '#a0a0a0' },
                            grid: { color: 'rgba(255,255,255,0.1)' }
                        },
                        y: {
                            ticks: { color: '#a0a0a0' },
                            grid: { color: 'rgba(255,255,255,0.1)' }
                        }
                    }
                }
            });

            // Latency chart
            if (latencyChart) {
                latencyChart.destroy();
            }

            latencyChart = new Chart(document.getElementById('latencyChart'), {
                type: 'bar',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Avg Latency (ms)',
                        data: latencies,
                        backgroundColor: 'rgba(103, 178, 111, 0.8)',
                        borderColor: 'rgba(103, 178, 111, 1)',
                        borderWidth: 1
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            labels: { color: '#fff' }
                        }
                    },
                    scales: {
                        x: {
                            ticks: { color: '#a0a0a0' },
                            grid: { color: 'rgba(255,255,255,0.1)' }
                        },
                        y: {
                            ticks: { color: '#a0a0a0' },
                            grid: { color: 'rgba(255,255,255,0.1)' }
                        }
                    }
                }
            });
        }

        function updateTable(benchmarks) {
            const tbody = document.querySelector('#resultsTable tbody');
            let html = '';

            for (const [name, bench] of Object.entries(benchmarks)) {
                const success = bench.successful_runs > 0;
                html += `
                    <tr>
                        <td>${name}</td>
                        <td>
                            <span class="status-badge ${success ? 'success' : 'failed'}">
                                ${success ? 'Passed' : 'Failed'}
                            </span>
                        </td>
                        <td>${bench.runs || 0}</td>
                        <td>${(bench.latency?.avg_ms || 0).toFixed(2)} ms</td>
                        <td>${formatNumber(bench.throughput?.avg_ops || 0)} ops/s</td>
                    </tr>
                `;
            }

            tbody.innerHTML = html || '<tr><td colspan="5">No results available</td></tr>';
        }

        function formatNumber(num) {
            if (num >= 1000000) return (num / 1000000).toFixed(1) + 'M';
            if (num >= 1000) return (num / 1000).toFixed(1) + 'K';
            return num.toFixed(0);
        }

        // Initial fetch
        fetchData();

        // Auto-refresh every 30 seconds
        setInterval(fetchData, 30000);
    </script>
</body>
</html>
'''


def create_app(results_dir: str = 'results') -> Flask:
    """Create the Flask application."""
    if not HAS_FLASK:
        raise ImportError("Flask is required. Install with: pip install flask flask-cors")

    app = Flask(__name__)
    CORS(app)

    results_path = Path(results_dir)
    aggregator = ResultsAggregator(str(results_path))

    @app.route('/')
    def index():
        """Serve the dashboard."""
        return render_template_string(DASHBOARD_TEMPLATE)

    @app.route('/api/summary')
    def api_summary():
        """Return aggregated benchmark summary."""
        # Reload results to get latest
        aggregator.suites = []
        aggregator.comparisons = {}
        aggregator.load_results()

        summary = {
            "generated_at": datetime.now().isoformat(),
            "summary": aggregator.get_summary(),
            "categories": aggregator.get_benchmark_categories(),
            "benchmarks": {
                name: aggregator.get_comparison(name)
                for name in aggregator.comparisons.keys()
            }
        }

        return jsonify(summary)

    @app.route('/api/benchmark/<name>')
    def api_benchmark(name: str):
        """Return details for a specific benchmark."""
        aggregator.suites = []
        aggregator.comparisons = {}
        aggregator.load_results()

        return jsonify(aggregator.get_comparison(name))

    @app.route('/charts/<path:filename>')
    def serve_chart(filename):
        """Serve chart images."""
        charts_dir = results_path.parent / 'charts'
        return send_from_directory(str(charts_dir), filename)

    return app


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Orochi DB Benchmark Dashboard Server')
    parser.add_argument('--results-dir', '-r', default='../results',
                        help='Directory containing result JSON files')
    parser.add_argument('--port', '-p', type=int, default=5000,
                        help='Server port (default: 5000)')
    parser.add_argument('--host', default='127.0.0.1',
                        help='Server host (default: 127.0.0.1)')
    parser.add_argument('--debug', action='store_true',
                        help='Enable debug mode')

    args = parser.parse_args()

    if not HAS_FLASK:
        print("Error: Flask is required for the web server")
        print("Install with: pip install flask flask-cors")
        sys.exit(1)

    app = create_app(args.results_dir)

    print(f"\nOrochi DB Benchmark Dashboard")
    print(f"=============================")
    print(f"Results directory: {args.results_dir}")
    print(f"Server running at: http://{args.host}:{args.port}")
    print(f"\nPress Ctrl+C to stop\n")

    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == '__main__':
    main()
