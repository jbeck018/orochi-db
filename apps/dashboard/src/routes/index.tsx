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
import type { Cluster } from "@/types";

export const Route = createFileRoute("/")({
  component: DashboardPage,
});

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
              Overview of your Orochi Cloud clusters
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
            <a href="https://docs.orochi.cloud" target="_blank" rel="noopener noreferrer">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <TrendingUp className="h-5 w-5 text-primary" />
                  Documentation
                </CardTitle>
                <CardDescription>
                  Learn how to use Orochi Cloud features
                </CardDescription>
              </CardHeader>
            </a>
          </Card>

          <Card className="hover:shadow-md transition-shadow cursor-pointer">
            <a href="https://support.orochi.cloud" target="_blank" rel="noopener noreferrer">
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
