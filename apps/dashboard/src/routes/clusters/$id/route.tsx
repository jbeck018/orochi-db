import * as React from "react";
import { createFileRoute, Link, Outlet } from "@tanstack/react-router";
import {
  Database,
  Settings,
  Play,
  Square,
  RefreshCw,
  AlertCircle,
  Table2,
  Zap,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { useToast } from "@/hooks/use-toast";
import {
  useCluster,
  useStartCluster,
  useStopCluster,
  useRestartCluster,
} from "@/hooks/api";
import { getStatusColor } from "@/lib/utils";

export const Route = createFileRoute("/clusters/$id")({
  component: ClusterLayout,
});

function ClusterLayout(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  // Fetch cluster data using TanStack Query
  const { data: clusterData, isLoading: isClusterLoading } = useCluster(clusterId);
  const cluster = clusterData?.data;

  // Mutations for cluster actions
  const startMutation = useStartCluster(clusterId);
  const stopMutation = useStopCluster(clusterId);
  const restartMutation = useRestartCluster(clusterId);

  const isActionLoading =
    startMutation.isPending ||
    stopMutation.isPending ||
    restartMutation.isPending;

  const handleAction = async (action: "start" | "stop" | "restart"): Promise<void> => {
    try {
      switch (action) {
        case "start":
          await startMutation.mutateAsync();
          toast({ title: "Cluster starting" });
          break;
        case "stop":
          await stopMutation.mutateAsync();
          toast({ title: "Cluster stopping" });
          break;
        case "restart":
          await restartMutation.mutateAsync();
          toast({ title: "Cluster restarting" });
          break;
      }
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Action failed",
        variant: "destructive",
      });
    }
  };

  if (isClusterLoading) {
    return (
      <DashboardLayout>
        <div className="space-y-6">
          <Skeleton className="h-10 w-64" />
          <div className="grid gap-4 md:grid-cols-4">
            {[1, 2, 3, 4].map((i) => (
              <Skeleton key={i} className="h-32" />
            ))}
          </div>
          <Skeleton className="h-[400px]" />
        </div>
      </DashboardLayout>
    );
  }

  if (!cluster) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <AlertCircle className="h-12 w-12 text-muted-foreground mb-4" />
          <h2 className="text-xl font-semibold">Cluster not found</h2>
          <p className="text-muted-foreground mb-4">
            The cluster you're looking for doesn't exist or has been deleted.
          </p>
          <Button asChild>
            <Link to="/clusters">Back to Clusters</Link>
          </Button>
        </div>
      </DashboardLayout>
    );
  }

  return (
    <DashboardLayout>
      <div className="space-y-6">
        {/* Header */}
        <div className="flex items-start justify-between">
          <div className="flex items-center gap-4">
            <div className="flex h-12 w-12 items-center justify-center rounded-lg bg-primary/10">
              <Database className="h-6 w-6 text-primary" />
            </div>
            <div>
              <h1 className="text-3xl font-bold tracking-tight">
                {cluster.name}
              </h1>
              <div className="flex items-center gap-2 mt-1">
                <Badge
                  variant="outline"
                  className={getStatusColor(cluster.status)}
                >
                  <span className="mr-1.5 h-2 w-2 rounded-full bg-current" />
                  {cluster.status.charAt(0).toUpperCase() + cluster.status.slice(1)}
                </Badge>
                <span className="text-sm text-muted-foreground">
                  {cluster.config.provider.toUpperCase()} - {cluster.config.region}
                </span>
              </div>
            </div>
          </div>
          <div className="flex items-center gap-2">
            {cluster.status === "stopped" && (
              <Button
                onClick={() => handleAction("start")}
                disabled={isActionLoading}
              >
                <Play className="mr-2 h-4 w-4" />
                Start
              </Button>
            )}
            {cluster.status === "running" && (
              <>
                <Button
                  variant="outline"
                  onClick={() => handleAction("restart")}
                  disabled={isActionLoading}
                >
                  <RefreshCw className="mr-2 h-4 w-4" />
                  Restart
                </Button>
                <Button
                  variant="outline"
                  onClick={() => handleAction("stop")}
                  disabled={isActionLoading}
                >
                  <Square className="mr-2 h-4 w-4" />
                  Stop
                </Button>
              </>
            )}
            <Button variant="outline" asChild>
              <Link to="/clusters/$id/settings" params={{ id: clusterId }}>
                <Settings className="mr-2 h-4 w-4" />
                Settings
              </Link>
            </Button>
          </div>
        </div>

        {/* Data Browser - Primary CTA */}
        <Card className="border-primary/50 bg-primary/5">
          <CardContent className="flex items-center justify-between py-4">
            <div>
              <h3 className="font-semibold">Data Browser</h3>
              <p className="text-sm text-muted-foreground">
                Browse tables, run queries, and explore your data
              </p>
            </div>
            <Button asChild>
              <Link to="/clusters/$id/data" params={{ id: clusterId }}>
                <Table2 className="mr-2 h-4 w-4" />
                Open Data Browser
              </Link>
            </Button>
          </CardContent>
        </Card>

        {/* Quick Links to Orochi Features */}
        <Card>
          <CardHeader>
            <CardTitle>Orochi DB Features</CardTitle>
            <CardDescription>
              Explore HTAP capabilities: sharding, time-series, columnar storage, and more
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="grid gap-3 md:grid-cols-3">
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/sharding" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  Sharding
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/timeseries" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  Time-Series
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/columnar" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  Columnar Storage
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/topology" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  Topology
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/pipelines" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  Pipelines
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/cdc" params={{ id: clusterId }}>
                  <Database className="mr-2 h-4 w-4" />
                  CDC
                </Link>
              </Button>
              <Button variant="outline" className="justify-start" asChild>
                <Link to="/clusters/$id/pooler" params={{ id: clusterId }}>
                  <Zap className="mr-2 h-4 w-4" />
                  Connection Pooler
                </Link>
              </Button>
            </div>
          </CardContent>
        </Card>

        {/* Child route content renders here */}
        <Outlet />
      </div>
    </DashboardLayout>
  );
}
