import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Plus,
  Search,
  Filter,
  Grid,
  List,
  RefreshCw,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Card,
  CardContent,
  CardHeader,
} from "@/components/ui/card";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Skeleton } from "@/components/ui/skeleton";
import { ClusterCard } from "@/components/clusters/cluster-card";
import { clusterApi } from "@/lib/api";
import { cn } from "@/lib/utils";
import type { Cluster, ClusterMetrics, ClusterStatus } from "@/types";

export const Route = createFileRoute("/clusters/")({
  component: ClustersPage,
});

type ViewMode = "grid" | "list";

function ClustersPage(): React.JSX.Element {
  const [clusters, setClusters] = React.useState<Cluster[]>([]);
  const [metrics, setMetrics] = React.useState<Record<string, ClusterMetrics>>({});
  const [isLoading, setIsLoading] = React.useState(true);
  const [isRefreshing, setIsRefreshing] = React.useState(false);
  const [searchQuery, setSearchQuery] = React.useState("");
  const [statusFilter, setStatusFilter] = React.useState<ClusterStatus | "all">("all");
  const [viewMode, setViewMode] = React.useState<ViewMode>("grid");

  const fetchData = React.useCallback(async (showRefresh = false): Promise<void> => {
    if (showRefresh) setIsRefreshing(true);
    try {
      const response = await clusterApi.list(1, 100);
      setClusters(response.data);

      // Fetch metrics for running clusters
      const metricsMap: Record<string, ClusterMetrics> = {};
      await Promise.all(
        response.data
          .filter((c) => c.status === "running")
          .map(async (cluster) => {
            try {
              const metricsResponse = await clusterApi.getMetrics(cluster.id);
              metricsMap[cluster.id] = metricsResponse.data;
            } catch {
              // Ignore metrics errors
            }
          })
      );
      setMetrics(metricsMap);
    } catch (error) {
      console.error("Failed to fetch clusters:", error);
    } finally {
      setIsLoading(false);
      setIsRefreshing(false);
    }
  }, []);

  React.useEffect(() => {
    fetchData();
    const interval = setInterval(() => fetchData(), 30000);
    return () => clearInterval(interval);
  }, [fetchData]);

  const filteredClusters = clusters.filter((cluster) => {
    const matchesSearch = cluster.name
      .toLowerCase()
      .includes(searchQuery.toLowerCase());
    const matchesStatus =
      statusFilter === "all" || cluster.status === statusFilter;
    return matchesSearch && matchesStatus;
  });

  return (
    <DashboardLayout>
      <div className="space-y-6">
        {/* Header */}
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-3xl font-bold tracking-tight">Clusters</h1>
            <p className="text-muted-foreground">
              Manage your PostgreSQL HTAP clusters
            </p>
          </div>
          <Button asChild>
            <Link to="/clusters/new">
              <Plus className="mr-2 h-4 w-4" />
              New Cluster
            </Link>
          </Button>
        </div>

        {/* Filters */}
        <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
          <div className="flex flex-1 items-center gap-2">
            <div className="relative flex-1 max-w-sm">
              <Search className="absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
              <Input
                placeholder="Search clusters..."
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                className="pl-9"
              />
            </div>
            <Select
              value={statusFilter}
              onValueChange={(value) =>
                setStatusFilter(value as ClusterStatus | "all")
              }
            >
              <SelectTrigger className="w-32">
                <Filter className="mr-2 h-4 w-4" />
                <SelectValue placeholder="Status" />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="all">All</SelectItem>
                <SelectItem value="running">Running</SelectItem>
                <SelectItem value="stopped">Stopped</SelectItem>
                <SelectItem value="creating">Creating</SelectItem>
                <SelectItem value="error">Error</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <div className="flex items-center gap-2">
            <Button
              variant="outline"
              size="icon"
              onClick={() => fetchData(true)}
              disabled={isRefreshing}
            >
              <RefreshCw
                className={cn("h-4 w-4", isRefreshing && "animate-spin")}
              />
            </Button>
            <div className="flex items-center rounded-md border">
              <Button
                variant={viewMode === "grid" ? "secondary" : "ghost"}
                size="icon"
                className="rounded-r-none"
                onClick={() => setViewMode("grid")}
              >
                <Grid className="h-4 w-4" />
              </Button>
              <Button
                variant={viewMode === "list" ? "secondary" : "ghost"}
                size="icon"
                className="rounded-l-none"
                onClick={() => setViewMode("list")}
              >
                <List className="h-4 w-4" />
              </Button>
            </div>
          </div>
        </div>

        {/* Cluster List */}
        {isLoading ? (
          <div
            className={cn(
              "grid gap-4",
              viewMode === "grid" ? "md:grid-cols-2 lg:grid-cols-3" : "grid-cols-1"
            )}
          >
            {[1, 2, 3, 4, 5, 6].map((i) => (
              <Card key={i}>
                <CardHeader>
                  <Skeleton className="h-6 w-32" />
                  <Skeleton className="h-4 w-48" />
                </CardHeader>
                <CardContent>
                  <Skeleton className="h-24 w-full" />
                </CardContent>
              </Card>
            ))}
          </div>
        ) : filteredClusters.length > 0 ? (
          <div
            className={cn(
              "grid gap-4",
              viewMode === "grid" ? "md:grid-cols-2 lg:grid-cols-3" : "grid-cols-1"
            )}
          >
            {filteredClusters.map((cluster) => (
              <ClusterCard
                key={cluster.id}
                cluster={cluster}
                metrics={metrics[cluster.id]}
                onRefresh={() => fetchData()}
              />
            ))}
          </div>
        ) : (
          <Card>
            <CardContent className="flex flex-col items-center justify-center py-10">
              <Database className="h-12 w-12 text-muted-foreground mb-4" />
              {searchQuery || statusFilter !== "all" ? (
                <>
                  <h3 className="text-lg font-semibold">No clusters found</h3>
                  <p className="text-sm text-muted-foreground mb-4">
                    Try adjusting your search or filter criteria
                  </p>
                  <Button
                    variant="outline"
                    onClick={() => {
                      setSearchQuery("");
                      setStatusFilter("all");
                    }}
                  >
                    Clear filters
                  </Button>
                </>
              ) : (
                <>
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
                </>
              )}
            </CardContent>
          </Card>
        )}

        {/* Results count */}
        {!isLoading && filteredClusters.length > 0 && (
          <p className="text-sm text-muted-foreground">
            Showing {filteredClusters.length} of {clusters.length} clusters
          </p>
        )}
      </div>
    </DashboardLayout>
  );
}
