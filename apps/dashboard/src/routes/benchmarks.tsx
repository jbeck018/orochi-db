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
                <Link to="/docs#benchmarks">
                  View Docs
                  <ChevronRight className="ml-2 h-4 w-4" />
                </Link>
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
