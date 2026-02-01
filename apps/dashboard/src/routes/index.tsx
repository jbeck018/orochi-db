import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Plus,
  Activity,
  HardDrive,
  Users,
  TrendingUp,
  AlertCircle,
  Zap,
  Clock,
  BarChart3,
  Layers,
  GitBranch,
  ArrowRight,
  Check,
  ChevronRight,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { ClusterCard } from "@/components/clusters/cluster-card";
import { clusterApi, configApi, type SystemHealth } from "@/lib/api";
import { formatBytes } from "@/lib/utils";
import { isAuthenticated } from "@/lib/auth";
import howleropsLogo from "@/src/assets/howlerops-icon.png";
import type { Cluster } from "@/types";

export const Route = createFileRoute("/")({
  component: IndexPage,
});

// Main index page - shows landing for guests, dashboard for authenticated users
function IndexPage(): React.JSX.Element {
  const [authState, setAuthState] = React.useState<'loading' | 'authenticated' | 'unauthenticated'>('loading');

  React.useEffect(() => {
    // Check auth status on client only
    const authenticated = isAuthenticated();
    setAuthState(authenticated ? 'authenticated' : 'unauthenticated');
  }, []);

  // During SSR and initial client render, show landing page
  // This ensures no hydration mismatch and no redirect for unauthenticated users
  if (authState === 'loading' || authState === 'unauthenticated') {
    return <LandingPage />;
  }

  // Only render DashboardPage when we're certain the user is authenticated
  return <DashboardPage />;
}

interface DashboardStats {
  totalClusters: number;
  runningClusters: number;
  totalStorage: number;
  totalConnections: number;
}

function DashboardPage(): React.JSX.Element {
  const [clusters, setClusters] = React.useState<Cluster[]>([]);
  const [stats, setStats] = React.useState<DashboardStats | null>(null);
  const [systemHealth, setSystemHealth] = React.useState<SystemHealth | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      // Fetch clusters and system health in parallel
      const [response, healthResponse] = await Promise.all([
        clusterApi.list(1, 100),
        configApi.getSystemHealth().catch(() => null),
      ]);

      setClusters(response.data);
      if (healthResponse) {
        setSystemHealth(healthResponse.data);
      }

      // Calculate stats
      const totalClusters = response.data.length;
      const runningClusters = response.data.filter((c) => c.status === "running").length;

      // Fetch metrics for running clusters
      let totalStorage = 0;
      let totalConnections = 0;

      await Promise.all(
        response.data
          .filter((c) => c.status === "running")
          .slice(0, 5)
          .map(async (cluster) => {
            try {
              const metricsResponse = await clusterApi.getMetrics(cluster.id);
              totalStorage += metricsResponse.data.storage.usedBytes;
              totalConnections += metricsResponse.data.connections.active;
            } catch {
              // Ignore metrics errors
            }
          })
      );
      setStats({
        totalClusters,
        runningClusters,
        totalStorage,
        totalConnections,
      });
    } catch (error) {
      console.error("Failed to fetch dashboard data:", error);
    } finally {
      setIsLoading(false);
    }
  }, []);

  React.useEffect(() => {
    fetchData();
    // Poll every 30 seconds
    const interval = setInterval(fetchData, 30000);
    return () => clearInterval(interval);
  }, [fetchData]);

  const recentClusters = clusters.slice(0, 4);

  return (
    <DashboardLayout>
      <div className="space-y-8">
        {/* Header */}
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-3xl font-bold tracking-tight">Dashboard</h1>
            <p className="text-muted-foreground">
              Overview of your OrochiDB clusters
            </p>
          </div>
          <Button asChild>
            <Link to="/clusters/new">
              <Plus className="mr-2 h-4 w-4" />
              New Cluster
            </Link>
          </Button>
        </div>

        {/* Stats Cards */}
        <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-4">
          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">
                Total Clusters
              </CardTitle>
              <Database className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              {isLoading ? (
                <Skeleton className="h-8 w-16" />
              ) : (
                <>
                  <div className="text-2xl font-bold">
                    {stats?.totalClusters ?? 0}
                  </div>
                  <p className="text-xs text-muted-foreground">
                    {stats?.runningClusters ?? 0} running
                  </p>
                </>
              )}
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">
                Active Connections
              </CardTitle>
              <Users className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              {isLoading ? (
                <Skeleton className="h-8 w-16" />
              ) : (
                <>
                  <div className="text-2xl font-bold">
                    {stats?.totalConnections ?? 0}
                  </div>
                  <p className="text-xs text-muted-foreground">
                    Across all clusters
                  </p>
                </>
              )}
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">
                Storage Used
              </CardTitle>
              <HardDrive className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              {isLoading ? (
                <Skeleton className="h-8 w-16" />
              ) : (
                <>
                  <div className="text-2xl font-bold">
                    {formatBytes(stats?.totalStorage ?? 0)}
                  </div>
                  <p className="text-xs text-muted-foreground">
                    Total across clusters
                  </p>
                </>
              )}
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">
                System Health
              </CardTitle>
              <Activity className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              {isLoading ? (
                <Skeleton className="h-8 w-16" />
              ) : (
                <>
                  <div className={`text-2xl font-bold ${
                    systemHealth?.status === "operational" ? "text-green-600" :
                    systemHealth?.status === "degraded" ? "text-yellow-600" :
                    systemHealth?.status === "outage" ? "text-red-600" : "text-green-600"
                  }`}>
                    {systemHealth?.status === "operational" ? "Healthy" :
                     systemHealth?.status === "degraded" ? "Degraded" :
                     systemHealth?.status === "outage" ? "Outage" : "Healthy"}
                  </div>
                  <p className="text-xs text-muted-foreground">
                    {systemHealth?.status === "operational" ? "All systems operational" :
                     systemHealth?.status === "degraded" ? "Some services affected" :
                     systemHealth?.status === "outage" ? "Service disruption" : "All systems operational"}
                  </p>
                </>
              )}
            </CardContent>
          </Card>
        </div>

        {/* Recent Clusters */}
        <div>
          <div className="flex items-center justify-between mb-4">
            <h2 className="text-xl font-semibold">Recent Clusters</h2>
            <Button variant="outline" size="sm" asChild>
              <Link to="/clusters">View all</Link>
            </Button>
          </div>

          {isLoading ? (
            <div className="grid gap-4 md:grid-cols-2">
              {[1, 2, 3, 4].map((i) => (
                <Card key={i}>
                  <CardHeader>
                    <Skeleton className="h-6 w-32" />
                    <Skeleton className="h-4 w-48" />
                  </CardHeader>
                  <CardContent>
                    <Skeleton className="h-20 w-full" />
                  </CardContent>
                </Card>
              ))}
            </div>
          ) : recentClusters.length > 0 ? (
            <div className="grid gap-4 md:grid-cols-2">
              {recentClusters.map((cluster) => (
                <ClusterCard
                  key={cluster.id}
                  cluster={cluster}
                  onRefresh={fetchData}
                />
              ))}
            </div>
          ) : (
            <Card>
              <CardContent className="flex flex-col items-center justify-center py-10">
                <Database className="h-12 w-12 text-muted-foreground mb-4" />
                <h3 className="text-lg font-semibold">No clusters yet</h3>
                <p className="text-sm text-muted-foreground mb-4">
                  Create your first cluster to get started
                </p>
                <Button asChild>
                  <Link to="/clusters/new">
                    <Plus className="mr-2 h-4 w-4" />
                    Create Cluster
                  </Link>
                </Button>
              </CardContent>
            </Card>
          )}
        </div>

        {/* Quick Actions */}
        <div className="grid gap-4 md:grid-cols-3">
          <Card className="hover:shadow-md transition-shadow cursor-pointer">
            <Link to="/clusters/new">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Plus className="h-5 w-5 text-primary" />
                  Create Cluster
                </CardTitle>
                <CardDescription>
                  Deploy a new PostgreSQL HTAP cluster
                </CardDescription>
              </CardHeader>
            </Link>
          </Card>

          <Card className="hover:shadow-md transition-shadow cursor-pointer">
            <a href="https://docs.howlerops.com" target="_blank" rel="noopener noreferrer">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <TrendingUp className="h-5 w-5 text-primary" />
                  Documentation
                </CardTitle>
                <CardDescription>
                  Learn how to use OrochiDB features
                </CardDescription>
              </CardHeader>
            </a>
          </Card>

          <Card className="hover:shadow-md transition-shadow cursor-pointer">
            <a href="https://support.howlerops.com" target="_blank" rel="noopener noreferrer">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <AlertCircle className="h-5 w-5 text-primary" />
                  Get Support
                </CardTitle>
                <CardDescription>
                  Contact our team for assistance
                </CardDescription>
              </CardHeader>
            </a>
          </Card>
        </div>
      </div>
    </DashboardLayout>
  );
}

// Landing page features data
const landingFeatures = [
  {
    icon: Database,
    title: "Automatic Sharding",
    description:
      "Hash-based horizontal distribution across nodes. Scale your data effortlessly without manual intervention.",
  },
  {
    icon: Clock,
    title: "Time-Series Optimization",
    description:
      "Automatic time-based partitioning with chunks for efficient time-series workloads.",
  },
  {
    icon: Layers,
    title: "Columnar Storage",
    description:
      "Column-oriented format with advanced compression for blazing-fast analytics queries.",
  },
  {
    icon: BarChart3,
    title: "Tiered Storage",
    description:
      "Hot/warm/cold/frozen data lifecycle with S3 integration for cost-effective storage.",
  },
  {
    icon: Zap,
    title: "Vector & AI Workloads",
    description:
      "SIMD-optimized vector operations and similarity search for AI/ML applications.",
  },
  {
    icon: GitBranch,
    title: "Change Data Capture",
    description:
      "Stream database changes to external systems in real-time with CDC support.",
  },
];

const landingUseCases = [
  {
    title: "Real-Time Analytics",
    description:
      "Run complex analytical queries on live transactional data without impacting your OLTP workloads.",
    icon: BarChart3,
  },
  {
    title: "Time-Series Data",
    description:
      "Store and query billions of time-series data points with automatic partitioning and compression.",
    icon: Clock,
  },
  {
    title: "AI/ML Applications",
    description:
      "Build intelligent applications with native vector search and similarity matching capabilities.",
    icon: Zap,
  },
];

// Landing page component for unauthenticated users
function LandingPage(): React.JSX.Element {
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
              to="/"
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
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
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
      <section className="container mx-auto px-4 py-20 md:py-32">
        <div className="flex flex-col items-center text-center max-w-4xl mx-auto">
          <div className="inline-flex items-center gap-2 rounded-full border px-4 py-1.5 text-sm mb-6">
            <span className="text-primary font-medium">New</span>
            <span className="text-muted-foreground">
              PostgreSQL 17 support now available
            </span>
            <ChevronRight className="h-4 w-4 text-muted-foreground" />
          </div>
          <h1 className="text-4xl md:text-6xl font-bold tracking-tight mb-6">
            <span className="text-primary">OrochiDB</span> - PostgreSQL for{" "}
            Modern Workloads
          </h1>
          <p className="text-xl text-muted-foreground mb-8 max-w-2xl">
            OrochiDB combines OLTP and OLAP in a single PostgreSQL database.
            Automatic sharding, time-series optimization, columnar storage, and
            vector search - fully managed by HowlerOps.
          </p>
          <div className="flex flex-col sm:flex-row gap-4">
            <Button size="lg" asChild>
              <Link to="/register">
                Start Free Trial
                <ArrowRight className="ml-2 h-4 w-4" />
              </Link>
            </Button>
            <Button size="lg" variant="outline" asChild>
              <Link to="/docs">View Documentation</Link>
            </Button>
          </div>
          <p className="text-sm text-muted-foreground mt-4">
            No credit card required. Free tier includes 1GB storage.
          </p>
        </div>
      </section>

      {/* Features Grid */}
      <section className="border-t bg-muted/50 py-20">
        <div className="container mx-auto px-4">
          <div className="text-center mb-12">
            <h2 className="text-3xl font-bold tracking-tight mb-4">
              Everything You Need for HTAP Workloads
            </h2>
            <p className="text-lg text-muted-foreground max-w-2xl mx-auto">
              Built on PostgreSQL with extensions for modern hybrid
              transactional/analytical processing.
            </p>
          </div>
          <div className="grid gap-6 md:grid-cols-2 lg:grid-cols-3">
            {landingFeatures.map((feature) => (
              <Card key={feature.title} className="bg-background">
                <CardHeader>
                  <feature.icon className="h-10 w-10 text-primary mb-2" />
                  <CardTitle>{feature.title}</CardTitle>
                </CardHeader>
                <CardContent>
                  <CardDescription className="text-base">
                    {feature.description}
                  </CardDescription>
                </CardContent>
              </Card>
            ))}
          </div>
        </div>
      </section>

      {/* Use Cases */}
      <section className="py-20">
        <div className="container mx-auto px-4">
          <div className="text-center mb-12">
            <h2 className="text-3xl font-bold tracking-tight mb-4">
              Built for Your Use Case
            </h2>
            <p className="text-lg text-muted-foreground max-w-2xl mx-auto">
              Whether you need real-time analytics, time-series storage, or AI
              capabilities, OrochiDB has you covered.
            </p>
          </div>
          <div className="grid gap-8 md:grid-cols-3">
            {landingUseCases.map((useCase) => (
              <div
                key={useCase.title}
                className="flex flex-col items-center text-center p-6"
              >
                <div className="h-16 w-16 rounded-full bg-primary/10 flex items-center justify-center mb-4">
                  <useCase.icon className="h-8 w-8 text-primary" />
                </div>
                <h3 className="text-xl font-semibold mb-2">{useCase.title}</h3>
                <p className="text-muted-foreground">{useCase.description}</p>
              </div>
            ))}
          </div>
        </div>
      </section>

      {/* Stats Section */}
      <section className="border-t border-b bg-muted/50 py-16">
        <div className="container mx-auto px-4">
          <div className="grid gap-8 md:grid-cols-4 text-center">
            <div>
              <div className="text-4xl font-bold text-primary mb-2">99.99%</div>
              <div className="text-muted-foreground">Uptime SLA</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">10x</div>
              <div className="text-muted-foreground">Faster Analytics</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">80%</div>
              <div className="text-muted-foreground">Storage Savings</div>
            </div>
            <div>
              <div className="text-4xl font-bold text-primary mb-2">24/7</div>
              <div className="text-muted-foreground">Expert Support</div>
            </div>
          </div>
        </div>
      </section>

      {/* Why Orochi */}
      <section className="py-20">
        <div className="container mx-auto px-4">
          <div className="grid gap-12 md:grid-cols-2 items-center">
            <div>
              <h2 className="text-3xl font-bold tracking-tight mb-6">
                Why Choose OrochiDB?
              </h2>
              <div className="space-y-4">
                {[
                  "Full PostgreSQL compatibility - use your existing tools and queries",
                  "Automatic scaling based on workload demands",
                  "Enterprise-grade security with encryption at rest and in transit",
                  "Multi-region deployments for global applications",
                  "Integrated monitoring and alerting",
                  "Instant point-in-time recovery",
                ].map((item, index) => (
                  <div key={index} className="flex items-start gap-3">
                    <Check className="h-5 w-5 text-primary mt-0.5 flex-shrink-0" />
                    <span>{item}</span>
                  </div>
                ))}
              </div>
              <Button className="mt-8" asChild>
                <Link to="/register">
                  Start Building Today
                  <ArrowRight className="ml-2 h-4 w-4" />
                </Link>
              </Button>
            </div>
            <div className="relative">
              <div className="bg-muted rounded-lg p-6 font-mono text-sm">
                <div className="text-muted-foreground mb-2">
                  -- Create a distributed hypertable
                </div>
                <div className="text-foreground">
                  <span className="text-primary">SELECT</span> create_hypertable(
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-yellow-500">'events'</span>,
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-yellow-500">'timestamp'</span>,
                </div>
                <div className="text-foreground pl-4">
                  chunk_time_interval =&gt;{" "}
                  <span className="text-yellow-500">INTERVAL '1 day'</span>
                </div>
                <div className="text-foreground">);</div>
                <div className="text-muted-foreground mt-4 mb-2">
                  -- Enable columnar compression
                </div>
                <div className="text-foreground">
                  <span className="text-primary">ALTER TABLE</span> events
                </div>
                <div className="text-foreground pl-4">
                  <span className="text-primary">SET</span> (
                </div>
                <div className="text-foreground pl-8">
                  orochi.columnar_enabled ={" "}
                  <span className="text-green-500">true</span>,
                </div>
                <div className="text-foreground pl-8">
                  orochi.compression = <span className="text-yellow-500">'zstd'</span>
                </div>
                <div className="text-foreground pl-4">);</div>
              </div>
            </div>
          </div>
        </div>
      </section>

      {/* CTA Section */}
      <section className="border-t bg-primary text-primary-foreground py-20">
        <div className="container mx-auto px-4 text-center">
          <h2 className="text-3xl font-bold tracking-tight mb-4">
            Ready to Get Started?
          </h2>
          <p className="text-lg opacity-90 mb-8 max-w-2xl mx-auto">
            Deploy your first OrochiDB cluster in minutes. Start with our
            free tier and scale as you grow.
          </p>
          <div className="flex flex-col sm:flex-row gap-4 justify-center">
            <Button size="lg" variant="secondary" asChild>
              <Link to="/register">
                Create Free Account
                <ArrowRight className="ml-2 h-4 w-4" />
              </Link>
            </Button>
            <Button
              size="lg"
              variant="outline"
              className="bg-transparent border-primary-foreground text-primary-foreground hover:bg-primary-foreground/10"
              asChild
            >
              <Link to="/pricing">View Pricing</Link>
            </Button>
          </div>
        </div>
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
                  <Link to="/" className="hover:text-foreground">
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
