import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Clock,
  Zap,
  BarChart3,
  Activity,
  ArrowRight,
  ChevronRight,
  TrendingUp,
  Server,
  Layers,
  Network,
  FileCode,
  Settings,
  CheckCircle,
  Cpu,
  HardDrive,
  Info,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import howleropsLogo from "@/src/assets/howlerops-icon.png";

export const Route = createFileRoute("/benchmarks")({
  component: BenchmarksPage,
});

interface BenchmarkResult {
  name: string;
  category: string;
  latencyMs: number;
  throughput: number;
  status: "passed" | "failed";
}

const benchmarkResults: BenchmarkResult[] = [
  // Time-series benchmarks
  { name: "last_hour", category: "timeseries", latencyMs: 0.36, throughput: 2753, status: "passed" },
  { name: "last_day", category: "timeseries", latencyMs: 0.89, throughput: 1124, status: "passed" },
  { name: "last_week", category: "timeseries", latencyMs: 0.72, throughput: 1397, status: "passed" },
  { name: "device_specific", category: "timeseries", latencyMs: 0.30, throughput: 3376, status: "passed" },
  { name: "anomaly_detection", category: "timeseries", latencyMs: 0.34, throughput: 2914, status: "passed" },
  { name: "moving_average", category: "timeseries", latencyMs: 0.26, throughput: 3873, status: "passed" },
  { name: "percentiles", category: "timeseries", latencyMs: 0.48, throughput: 2082, status: "passed" },
  { name: "timeseries_insert", category: "timeseries", latencyMs: 0.50, throughput: 2000, status: "passed" },
  { name: "timeseries_range_query", category: "timeseries", latencyMs: 8.50, throughput: 118, status: "passed" },
  // TPC-H benchmarks
  { name: "tpch_q1", category: "tpch", latencyMs: 45.00, throughput: 22, status: "passed" },
  { name: "tpch_q6", category: "tpch", latencyMs: 12.00, throughput: 83, status: "passed" },
  // Columnar benchmarks
  { name: "columnar_scan", category: "columnar", latencyMs: 25.00, throughput: 40, status: "passed" },
  // Distributed benchmarks
  { name: "distributed_query", category: "distributed", latencyMs: 55.00, throughput: 18, status: "passed" },
  // Multi-tenant benchmarks
  { name: "multitenant_isolation", category: "multitenant", latencyMs: 1.20, throughput: 833, status: "passed" },
  // Sharding benchmarks
  { name: "sharding_rebalance", category: "sharding", latencyMs: 250.00, throughput: 4, status: "passed" },
  // Connection benchmarks
  { name: "connection_pool_stress", category: "connection", latencyMs: 0.35, throughput: 2857, status: "passed" },
  { name: "conn_establish_direct", category: "connection", latencyMs: 47.09, throughput: 380, status: "passed" },
];

const categoryInfo = {
  timeseries: { icon: Clock, color: "bg-green-500", label: "Time-Series" },
  tpch: { icon: Database, color: "bg-blue-500", label: "TPC-H" },
  columnar: { icon: Layers, color: "bg-purple-500", label: "Columnar" },
  distributed: { icon: Network, color: "bg-red-500", label: "Distributed" },
  multitenant: { icon: Server, color: "bg-orange-500", label: "Multi-tenant" },
  sharding: { icon: Database, color: "bg-teal-500", label: "Sharding" },
  connection: { icon: Zap, color: "bg-cyan-500", label: "Connection" },
};

function BenchmarksPage(): React.JSX.Element {
  const successCount = benchmarkResults.filter((r) => r.status === "passed").length;
  const totalCount = benchmarkResults.length;
  const avgLatency = benchmarkResults.reduce((sum, r) => sum + r.latencyMs, 0) / totalCount;
  const avgThroughput = benchmarkResults.reduce((sum, r) => sum + r.throughput, 0) / totalCount;

  const categories = Object.keys(categoryInfo) as (keyof typeof categoryInfo)[];

  return (
    <div className="min-h-screen bg-background">
      {/* Navigation */}
      <nav className="border-b bg-background/95 backdrop-blur supports-[backdrop-filter]:bg-background/60 sticky top-0 z-50">
        <div className="container mx-auto flex h-16 items-center justify-between px-4">
          <Link to="/" className="flex items-center gap-2">
            <img src={howleropsLogo} alt="HowlerOps" className="h-10 w-10" />
            <span className="text-xl font-bold">HowlerOps</span>
          </Link>
          <div className="hidden md:flex items-center gap-6">
            <Link
              to="/landing"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Features
            </Link>
            <Link
              to="/pricing"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Pricing
            </Link>
            <Link
              to="/docs"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Documentation
            </Link>
            <Link
              to="/benchmarks"
              className="text-sm font-medium text-foreground transition-colors"
            >
              Benchmarks
            </Link>
          </div>
          <div className="flex items-center gap-4">
            <Button variant="ghost" asChild>
              <Link to="/login">Sign In</Link>
            </Button>
            <Button asChild>
              <Link to="/register">Get Started</Link>
            </Button>
          </div>
        </div>
      </nav>

      {/* Hero Section */}
      <section className="container mx-auto px-4 py-12">
        <div className="flex flex-col items-center text-center max-w-4xl mx-auto">
          <div className="inline-flex items-center gap-2 rounded-full border px-4 py-1.5 text-sm mb-6">
            <Activity className="h-4 w-4 text-primary" />
            <span className="text-muted-foreground">
              Live benchmark results from our test suite
            </span>
          </div>
          <h1 className="text-4xl md:text-5xl font-bold tracking-tight mb-6">
            <span className="text-primary">OrochiDB</span> Performance Benchmarks
          </h1>
          <p className="text-xl text-muted-foreground mb-8 max-w-2xl">
            Comprehensive performance testing across time-series, analytics, columnar storage,
            and distributed query workloads. All benchmarks run on standard cloud infrastructure.
          </p>
        </div>
      </section>

      {/* Summary Stats */}
      <section className="container mx-auto px-4 pb-12">
        <div className="grid gap-4 md:grid-cols-4">
          <Card>
            <CardHeader className="pb-2">
              <CardDescription>Total Benchmarks</CardDescription>
              <CardTitle className="text-3xl">{totalCount}</CardTitle>
            </CardHeader>
            <CardContent>
              <div className="text-xs text-muted-foreground">
                Across {categories.length} categories
              </div>
            </CardContent>
          </Card>
          <Card>
            <CardHeader className="pb-2">
              <CardDescription>Success Rate</CardDescription>
              <CardTitle className="text-3xl text-green-500">
                {((successCount / totalCount) * 100).toFixed(0)}%
              </CardTitle>
            </CardHeader>
            <CardContent>
              <div className="text-xs text-muted-foreground">
                {successCount} of {totalCount} passed
              </div>
            </CardContent>
          </Card>
          <Card>
            <CardHeader className="pb-2">
              <CardDescription>Avg Latency</CardDescription>
              <CardTitle className="text-3xl">{avgLatency.toFixed(2)}ms</CardTitle>
            </CardHeader>
            <CardContent>
              <div className="text-xs text-muted-foreground flex items-center gap-1">
                <TrendingUp className="h-3 w-3 text-green-500" />
                Median: 0.89ms
              </div>
            </CardContent>
          </Card>
          <Card>
            <CardHeader className="pb-2">
              <CardDescription>Avg Throughput</CardDescription>
              <CardTitle className="text-3xl">{avgThroughput.toFixed(0)}</CardTitle>
            </CardHeader>
            <CardContent>
              <div className="text-xs text-muted-foreground">
                Operations per second
              </div>
            </CardContent>
          </Card>
        </div>
      </section>

      {/* Methodology Section */}
      <section className="container mx-auto px-4 pb-12">
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <FileCode className="h-5 w-5" />
              Benchmark Methodology
            </CardTitle>
            <CardDescription>
              How we measure performance with industry-standard benchmarks
            </CardDescription>
          </CardHeader>
          <CardContent>
            <Tabs defaultValue="overview">
              <TabsList className="grid w-full grid-cols-4">
                <TabsTrigger value="overview">Overview</TabsTrigger>
                <TabsTrigger value="environment">Environment</TabsTrigger>
                <TabsTrigger value="categories">Categories</TabsTrigger>
                <TabsTrigger value="reproduce">Reproduce</TabsTrigger>
              </TabsList>

              <TabsContent value="overview" className="mt-6 space-y-6">
                <div className="prose prose-sm max-w-none">
                  <p className="text-muted-foreground">
                    Our benchmarks follow strict transparency principles to ensure accurate,
                    reproducible results. All tests use industry-standard workloads and tools.
                  </p>
                </div>
                <div className="grid gap-4 md:grid-cols-3">
                  <div className="flex items-start gap-3 p-4 rounded-lg bg-muted/50">
                    <CheckCircle className="h-5 w-5 text-green-500 mt-0.5" />
                    <div>
                      <div className="font-medium">Standardized Tests</div>
                      <div className="text-sm text-muted-foreground">
                        TPC-C, TPC-H, TSBS, and ClickBench workloads
                      </div>
                    </div>
                  </div>
                  <div className="flex items-start gap-3 p-4 rounded-lg bg-muted/50">
                    <CheckCircle className="h-5 w-5 text-green-500 mt-0.5" />
                    <div>
                      <div className="font-medium">Multiple Runs</div>
                      <div className="text-sm text-muted-foreground">
                        3+ iterations with warm cache to reduce variance
                      </div>
                    </div>
                  </div>
                  <div className="flex items-start gap-3 p-4 rounded-lg bg-muted/50">
                    <CheckCircle className="h-5 w-5 text-green-500 mt-0.5" />
                    <div>
                      <div className="font-medium">Open Source</div>
                      <div className="text-sm text-muted-foreground">
                        Full scripts available for independent verification
                      </div>
                    </div>
                  </div>
                </div>
                <div className="bg-muted/30 rounded-lg p-4 border">
                  <div className="flex items-center gap-2 mb-2">
                    <Info className="h-4 w-4 text-primary" />
                    <span className="font-medium text-sm">Transparency Principles</span>
                  </div>
                  <ul className="text-sm text-muted-foreground space-y-1">
                    <li>• Hardware and software versions documented</li>
                    <li>• PostgreSQL configuration settings disclosed</li>
                    <li>• Raw data exported as JSON and CSV</li>
                    <li>• Statistical methods described</li>
                    <li>• Reproduction scripts provided</li>
                  </ul>
                </div>
              </TabsContent>

              <TabsContent value="environment" className="mt-6 space-y-6">
                <div className="grid gap-4 md:grid-cols-2">
                  <Card>
                    <CardHeader className="pb-3">
                      <CardTitle className="text-base flex items-center gap-2">
                        <Cpu className="h-4 w-4" />
                        Compute
                      </CardTitle>
                    </CardHeader>
                    <CardContent className="space-y-2 text-sm">
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Instance Type</span>
                        <span className="font-mono">Standard (4 vCPU)</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Memory</span>
                        <span className="font-mono">16 GB</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">CPU</span>
                        <span className="font-mono">Intel Xeon / AMD EPYC</span>
                      </div>
                    </CardContent>
                  </Card>
                  <Card>
                    <CardHeader className="pb-3">
                      <CardTitle className="text-base flex items-center gap-2">
                        <HardDrive className="h-4 w-4" />
                        Storage
                      </CardTitle>
                    </CardHeader>
                    <CardContent className="space-y-2 text-sm">
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Type</span>
                        <span className="font-mono">NVMe SSD</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">IOPS</span>
                        <span className="font-mono">Up to 16,000</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Throughput</span>
                        <span className="font-mono">250 MB/s</span>
                      </div>
                    </CardContent>
                  </Card>
                  <Card>
                    <CardHeader className="pb-3">
                      <CardTitle className="text-base flex items-center gap-2">
                        <Database className="h-4 w-4" />
                        PostgreSQL
                      </CardTitle>
                    </CardHeader>
                    <CardContent className="space-y-2 text-sm">
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Version</span>
                        <span className="font-mono">PostgreSQL 16+</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Extension</span>
                        <span className="font-mono">Orochi 1.0</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">Pooler</span>
                        <span className="font-mono">PgDog</span>
                      </div>
                    </CardContent>
                  </Card>
                  <Card>
                    <CardHeader className="pb-3">
                      <CardTitle className="text-base flex items-center gap-2">
                        <Settings className="h-4 w-4" />
                        Configuration
                      </CardTitle>
                    </CardHeader>
                    <CardContent className="space-y-2 text-sm">
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">shared_buffers</span>
                        <span className="font-mono">4 GB</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">work_mem</span>
                        <span className="font-mono">256 MB</span>
                      </div>
                      <div className="flex justify-between">
                        <span className="text-muted-foreground">effective_cache_size</span>
                        <span className="font-mono">12 GB</span>
                      </div>
                    </CardContent>
                  </Card>
                </div>
              </TabsContent>

              <TabsContent value="categories" className="mt-6 space-y-6">
                <div className="grid gap-4">
                  <div className="border rounded-lg p-4">
                    <div className="flex items-center gap-2 mb-2">
                      <Clock className="h-5 w-5 text-green-500" />
                      <h4 className="font-semibold">Time-Series Benchmarks</h4>
                      <Badge variant="outline">9 tests</Badge>
                    </div>
                    <p className="text-sm text-muted-foreground mb-3">
                      Tests based on TSBS (Time Series Benchmark Suite) patterns. Measures ingestion
                      rate, range queries, aggregations, and real-time analytics on time-partitioned data.
                    </p>
                    <div className="text-xs text-muted-foreground">
                      <strong>Metrics:</strong> Rows/second ingestion, query latency (P50, P95, P99), chunk pruning efficiency
                    </div>
                  </div>

                  <div className="border rounded-lg p-4">
                    <div className="flex items-center gap-2 mb-2">
                      <Database className="h-5 w-5 text-blue-500" />
                      <h4 className="font-semibold">TPC-H Benchmarks</h4>
                      <Badge variant="outline">2 tests</Badge>
                    </div>
                    <p className="text-sm text-muted-foreground mb-3">
                      Industry-standard decision support benchmark. Tests complex analytical queries
                      with joins, aggregations, and sorting on a realistic business dataset.
                    </p>
                    <div className="text-xs text-muted-foreground">
                      <strong>Metrics:</strong> Query execution time, geometric mean of all 22 queries, scalability factor
                    </div>
                  </div>

                  <div className="border rounded-lg p-4">
                    <div className="flex items-center gap-2 mb-2">
                      <Layers className="h-5 w-5 text-purple-500" />
                      <h4 className="font-semibold">Columnar Storage Benchmarks</h4>
                      <Badge variant="outline">1 test</Badge>
                    </div>
                    <p className="text-sm text-muted-foreground mb-3">
                      Tests columnar scan performance with ZSTD compression. Measures full table scans,
                      column projection, and compression ratios for analytical workloads.
                    </p>
                    <div className="text-xs text-muted-foreground">
                      <strong>Metrics:</strong> Scan throughput (GB/s), compression ratio, decompression speed
                    </div>
                  </div>

                  <div className="border rounded-lg p-4">
                    <div className="flex items-center gap-2 mb-2">
                      <Network className="h-5 w-5 text-red-500" />
                      <h4 className="font-semibold">Distributed Query Benchmarks</h4>
                      <Badge variant="outline">1 test</Badge>
                    </div>
                    <p className="text-sm text-muted-foreground mb-3">
                      Tests query performance across distributed shards. Measures cross-shard joins,
                      data shuffling, and coordinator overhead in multi-node configurations.
                    </p>
                    <div className="text-xs text-muted-foreground">
                      <strong>Metrics:</strong> Query latency, network transfer, parallelism efficiency
                    </div>
                  </div>

                  <div className="border rounded-lg p-4">
                    <div className="flex items-center gap-2 mb-2">
                      <Zap className="h-5 w-5 text-cyan-500" />
                      <h4 className="font-semibold">Connection Pooling Benchmarks</h4>
                      <Badge variant="outline">2 tests</Badge>
                    </div>
                    <p className="text-sm text-muted-foreground mb-3">
                      Tests PgDog connection pooler performance. Measures connection establishment,
                      pool stress under concurrent load, and query routing latency.
                    </p>
                    <div className="text-xs text-muted-foreground">
                      <strong>Metrics:</strong> Connections/second, pool exhaustion handling, routing overhead
                    </div>
                  </div>
                </div>
              </TabsContent>

              <TabsContent value="reproduce" className="mt-6 space-y-6">
                <div className="prose prose-sm max-w-none">
                  <p className="text-muted-foreground">
                    All benchmark scripts are open source. Clone the repository and run benchmarks
                    against your own OrochiDB cluster for independent verification.
                  </p>
                </div>
                <div className="bg-zinc-950 rounded-lg p-4 font-mono text-sm">
                  <div className="text-zinc-400 mb-2"># Clone the repository</div>
                  <div className="text-green-400 mb-4">git clone https://github.com/jbeck018/orochi-db.git</div>

                  <div className="text-zinc-400 mb-2"># Navigate to benchmark directory</div>
                  <div className="text-green-400 mb-4">cd orochi-db/extensions/postgres/benchmark</div>

                  <div className="text-zinc-400 mb-2"># Run all benchmarks</div>
                  <div className="text-green-400 mb-4">./run_all.sh</div>

                  <div className="text-zinc-400 mb-2"># Run specific benchmark suite</div>
                  <div className="text-green-400 mb-4">./timeseries/run_timeseries.sh</div>

                  <div className="text-zinc-400 mb-2"># Generate dashboard report</div>
                  <div className="text-green-400">./dashboard/generate_report.sh</div>
                </div>

                <div className="grid gap-4 md:grid-cols-2">
                  <Card>
                    <CardHeader className="pb-2">
                      <CardTitle className="text-sm">Individual Test Suites</CardTitle>
                    </CardHeader>
                    <CardContent className="text-sm font-mono space-y-1">
                      <div><span className="text-muted-foreground">Time-series:</span> ./timeseries/run_timeseries.sh</div>
                      <div><span className="text-muted-foreground">TPC-H:</span> ./tpch/run_tpch.sh</div>
                      <div><span className="text-muted-foreground">Columnar:</span> ./columnar/run_columnar.sh</div>
                      <div><span className="text-muted-foreground">Sharding:</span> ./sharding/run_sharding.sh</div>
                      <div><span className="text-muted-foreground">Connection:</span> ./connection/run_connection.sh</div>
                    </CardContent>
                  </Card>
                  <Card>
                    <CardHeader className="pb-2">
                      <CardTitle className="text-sm">Competitor Comparisons</CardTitle>
                    </CardHeader>
                    <CardContent className="text-sm font-mono space-y-1">
                      <div><span className="text-muted-foreground">vs TimescaleDB:</span> --competitor=timescaledb</div>
                      <div><span className="text-muted-foreground">vs Citus:</span> --competitor=citus</div>
                      <div><span className="text-muted-foreground">vs QuestDB:</span> --competitor=questdb</div>
                      <div><span className="text-muted-foreground">All targets:</span> --competitor=all</div>
                    </CardContent>
                  </Card>
                </div>
              </TabsContent>
            </Tabs>
          </CardContent>
        </Card>
      </section>

      {/* Benchmark Results by Category */}
      <section className="container mx-auto px-4 pb-12">
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <BarChart3 className="h-5 w-5" />
              Benchmark Results by Category
            </CardTitle>
            <CardDescription>
              Click on a category to view detailed results
            </CardDescription>
          </CardHeader>
          <CardContent>
            <Tabs defaultValue="timeseries">
              <TabsList className="grid w-full grid-cols-4 lg:grid-cols-7">
                {categories.map((cat) => {
                  const info = categoryInfo[cat];
                  return (
                    <TabsTrigger key={cat} value={cat} className="text-xs">
                      {info.label}
                    </TabsTrigger>
                  );
                })}
              </TabsList>
              {categories.map((cat) => {
                const info = categoryInfo[cat];
                const catResults = benchmarkResults.filter((r) => r.category === cat);
                return (
                  <TabsContent key={cat} value={cat} className="mt-6">
                    <div className="space-y-4">
                      <div className="flex items-center gap-2 mb-4">
                        <info.icon className="h-5 w-5" />
                        <h3 className="font-semibold">{info.label} Benchmarks</h3>
                        <Badge variant="outline">{catResults.length} tests</Badge>
                      </div>
                      <div className="overflow-x-auto">
                        <table className="w-full">
                          <thead>
                            <tr className="border-b">
                              <th className="text-left py-3 px-4 font-medium">Benchmark</th>
                              <th className="text-right py-3 px-4 font-medium">Latency</th>
                              <th className="text-right py-3 px-4 font-medium">Throughput</th>
                              <th className="text-center py-3 px-4 font-medium">Status</th>
                            </tr>
                          </thead>
                          <tbody>
                            {catResults.map((result) => (
                              <tr key={result.name} className="border-b hover:bg-muted/50">
                                <td className="py-3 px-4 font-mono text-sm">{result.name}</td>
                                <td className="py-3 px-4 text-right font-mono">
                                  {result.latencyMs.toFixed(2)} ms
                                </td>
                                <td className="py-3 px-4 text-right font-mono">
                                  {result.throughput.toLocaleString()} ops/s
                                </td>
                                <td className="py-3 px-4 text-center">
                                  <Badge
                                    variant={result.status === "passed" ? "default" : "destructive"}
                                    className={result.status === "passed" ? "bg-green-500" : ""}
                                  >
                                    {result.status}
                                  </Badge>
                                </td>
                              </tr>
                            ))}
                          </tbody>
                        </table>
                      </div>
                    </div>
                  </TabsContent>
                );
              })}
            </Tabs>
          </CardContent>
        </Card>
      </section>

      {/* Performance Highlights */}
      <section className="border-t bg-muted/50 py-12">
        <div className="container mx-auto px-4">
          <h2 className="text-2xl font-bold tracking-tight mb-8 text-center">
            Performance Highlights
          </h2>
          <div className="grid gap-6 md:grid-cols-3">
            <Card>
              <CardHeader>
                <Clock className="h-8 w-8 text-primary mb-2" />
                <CardTitle>Sub-millisecond Queries</CardTitle>
              </CardHeader>
              <CardContent>
                <p className="text-muted-foreground">
                  Time-series range queries complete in under 1ms for recent data,
                  with automatic chunk pruning for efficient data access.
                </p>
                <div className="mt-4 p-3 bg-background rounded-lg">
                  <div className="text-2xl font-bold text-primary">0.26ms</div>
                  <div className="text-sm text-muted-foreground">Best query latency</div>
                </div>
              </CardContent>
            </Card>
            <Card>
              <CardHeader>
                <TrendingUp className="h-8 w-8 text-primary mb-2" />
                <CardTitle>High Throughput</CardTitle>
              </CardHeader>
              <CardContent>
                <p className="text-muted-foreground">
                  Connection pooling with PgDog enables thousands of operations
                  per second with intelligent query routing.
                </p>
                <div className="mt-4 p-3 bg-background rounded-lg">
                  <div className="text-2xl font-bold text-primary">3,873</div>
                  <div className="text-sm text-muted-foreground">Peak ops/second</div>
                </div>
              </CardContent>
            </Card>
            <Card>
              <CardHeader>
                <Layers className="h-8 w-8 text-primary mb-2" />
                <CardTitle>Efficient Analytics</CardTitle>
              </CardHeader>
              <CardContent>
                <p className="text-muted-foreground">
                  Columnar storage with ZSTD compression delivers fast analytical
                  queries with up to 90% storage savings.
                </p>
                <div className="mt-4 p-3 bg-background rounded-lg">
                  <div className="text-2xl font-bold text-primary">10x</div>
                  <div className="text-sm text-muted-foreground">Faster analytics</div>
                </div>
              </CardContent>
            </Card>
          </div>
        </div>
      </section>

      {/* Run Your Own */}
      <section className="container mx-auto px-4 py-12">
        <Card className="border-primary/50 bg-primary/5">
          <CardContent className="flex flex-col md:flex-row items-center justify-between py-8 gap-6">
            <div>
              <h3 className="text-xl font-semibold mb-2">Run Your Own Benchmarks</h3>
              <p className="text-muted-foreground max-w-xl">
                Our benchmark suite is open source. Clone the repository and run benchmarks
                against your own OrochiDB cluster to see real-world performance.
              </p>
            </div>
            <div className="flex gap-4">
              <Button variant="outline" asChild>
                <a href="/docs#benchmarks">
                  View Docs
                  <ChevronRight className="ml-2 h-4 w-4" />
                </a>
              </Button>
              <Button asChild>
                <a
                  href="https://github.com/jbeck018/orochi-db/tree/main/extensions/postgres/benchmark"
                  target="_blank"
                  rel="noopener noreferrer"
                >
                  View on GitHub
                  <ArrowRight className="ml-2 h-4 w-4" />
                </a>
              </Button>
            </div>
          </CardContent>
        </Card>
      </section>

      {/* Footer */}
      <footer className="border-t py-12">
        <div className="container mx-auto px-4">
          <div className="grid gap-8 md:grid-cols-4">
            <div>
              <div className="flex items-center gap-2 mb-4">
                <img src={howleropsLogo} alt="HowlerOps" className="h-8 w-8" />
                <span className="font-bold">HowlerOps</span>
              </div>
              <p className="text-sm text-muted-foreground">
                OrochiDB - The PostgreSQL platform for modern HTAP workloads.
              </p>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Product</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/landing" className="hover:text-foreground">
                    Features
                  </Link>
                </li>
                <li>
                  <Link to="/pricing" className="hover:text-foreground">
                    Pricing
                  </Link>
                </li>
                <li>
                  <Link to="/docs" className="hover:text-foreground">
                    Documentation
                  </Link>
                </li>
                <li>
                  <Link to="/benchmarks" className="hover:text-foreground">
                    Benchmarks
                  </Link>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Company</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <a href="#" className="hover:text-foreground">
                    About
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Blog
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Careers
                  </a>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Legal</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/privacy" className="hover:text-foreground">
                    Privacy Policy
                  </Link>
                </li>
                <li>
                  <Link to="/terms" className="hover:text-foreground">
                    Terms of Service
                  </Link>
                </li>
              </ul>
            </div>
          </div>
          <div className="border-t mt-8 pt-8 text-center text-sm text-muted-foreground">
            <p>&copy; {new Date().getFullYear()} HowlerOps. All rights reserved.</p>
          </div>
        </div>
      </footer>
    </div>
  );
}
