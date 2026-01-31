import * as React from "react";
import { createFileRoute, Link, useNavigate } from "@tanstack/react-router";
import {
  Copy,
  Trash2,
  HardDrive,
  Cpu,
  MemoryStick,
  Users,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Skeleton } from "@/components/ui/skeleton";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  MetricsChart,
  ConnectionsChart,
  QueryPerformanceChart,
} from "@/components/clusters/metrics-chart";
import { PoolerStatusCard } from "@/components/clusters/pooler-status-card";
import { useToast } from "@/hooks/use-toast";
import {
  useCluster,
  useClusterMetrics,
  useClusterMetricsHistory,
  useDeleteCluster,
} from "@/hooks/api";
import {
  formatBytes,
  formatDate,
  copyToClipboard,
} from "@/lib/utils";
import { Settings } from "lucide-react";

export const Route = createFileRoute("/clusters/$id/")({
  component: ClusterOverview,
});

function ClusterOverview(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const navigate = useNavigate();
  const { toast } = useToast();

  const [showDeleteDialog, setShowDeleteDialog] = React.useState(false);
  const [deleteConfirmation, setDeleteConfirmation] = React.useState("");
  const [metricsHistoryPeriod, setMetricsHistoryPeriod] = React.useState<"1h" | "6h" | "24h" | "7d" | "30d">("24h");

  // Fetch cluster data using TanStack Query
  const { data: clusterData, isLoading: isClusterLoading } = useCluster(clusterId);
  const cluster = clusterData?.data;

  // Fetch metrics only for running clusters
  const { data: metricsData } = useClusterMetrics(cluster?.status === "running" ? clusterId : "");
  const metrics = metricsData?.data;

  // Fetch metrics history
  const { data: metricsHistoryData } = useClusterMetricsHistory(
    cluster?.status === "running" ? clusterId : "",
    metricsHistoryPeriod
  );
  const metricsHistory = metricsHistoryData?.data;

  // Mutations for cluster actions
  const deleteMutation = useDeleteCluster();

  const isActionLoading = deleteMutation.isPending;

  const handleDelete = async (): Promise<void> => {
    if (deleteConfirmation !== cluster?.name) return;

    try {
      await deleteMutation.mutateAsync(clusterId);
      toast({ title: "Cluster deleted" });
      navigate({ to: "/clusters" });
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to delete cluster",
        variant: "destructive",
      });
    } finally {
      setShowDeleteDialog(false);
    }
  };

  const handleCopyConnectionString = async (): Promise<void> => {
    if (cluster?.connectionString) {
      await copyToClipboard(cluster.connectionString);
      toast({ title: "Copied to clipboard" });
    }
  };

  const handlePeriodChange = async (period: "1h" | "6h" | "24h" | "7d" | "30d"): Promise<void> => {
    setMetricsHistoryPeriod(period);
  };

  // Parent layout handles main loading and not-found states
  // Show minimal loading for tab content
  if (isClusterLoading || !cluster) {
    return (
      <div className="space-y-4">
        <div className="grid gap-4 md:grid-cols-4">
          {[1, 2, 3, 4].map((i) => (
            <Skeleton key={i} className="h-32" />
          ))}
        </div>
        <Skeleton className="h-[400px]" />
      </div>
    );
  }

  return (
    <div className="space-y-6">
      {/* Quick Stats */}
      {cluster.status === "running" && metrics && (
        <div className="grid gap-4 md:grid-cols-4">
          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">CPU Usage</CardTitle>
              <Cpu className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              <div className="text-2xl font-bold">
                {metrics.cpu.usagePercent.toFixed(1)}%
              </div>
              <Progress
                value={metrics.cpu.usagePercent}
                className="h-2 mt-2"
              />
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">Memory</CardTitle>
              <MemoryStick className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              <div className="text-2xl font-bold">
                {metrics.memory.usagePercent.toFixed(1)}%
              </div>
              <Progress
                value={metrics.memory.usagePercent}
                className="h-2 mt-2"
              />
              <p className="text-xs text-muted-foreground mt-1">
                {formatBytes(metrics.memory.usedBytes)} / {formatBytes(metrics.memory.totalBytes)}
              </p>
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">Storage</CardTitle>
              <HardDrive className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              <div className="text-2xl font-bold">
                {metrics.storage.usagePercent.toFixed(1)}%
              </div>
              <Progress
                value={metrics.storage.usagePercent}
                className="h-2 mt-2"
              />
              <p className="text-xs text-muted-foreground mt-1">
                {formatBytes(metrics.storage.usedBytes)} / {formatBytes(metrics.storage.totalBytes)}
              </p>
            </CardContent>
          </Card>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-sm font-medium">Connections</CardTitle>
              <Users className="h-4 w-4 text-muted-foreground" />
            </CardHeader>
            <CardContent>
              <div className="text-2xl font-bold">
                {metrics.connections.active}
              </div>
              <Progress
                value={(metrics.connections.active / metrics.connections.max) * 100}
                className="h-2 mt-2"
              />
              <p className="text-xs text-muted-foreground mt-1">
                {metrics.connections.idle} idle / {metrics.connections.max} max
              </p>
            </CardContent>
          </Card>
        </div>
      )}

      {/* Tabs */}
      <Tabs defaultValue="overview" className="space-y-4">
        <TabsList>
          <TabsTrigger value="overview">Overview</TabsTrigger>
          <TabsTrigger value="metrics">Metrics</TabsTrigger>
          <TabsTrigger value="connection">Connection</TabsTrigger>
        </TabsList>

        <TabsContent value="overview" className="space-y-4">
          <div className="grid gap-4 md:grid-cols-2">
            <Card>
              <CardHeader>
                <CardTitle>Cluster Information</CardTitle>
              </CardHeader>
              <CardContent className="space-y-4">
                <div className="grid grid-cols-2 gap-4 text-sm">
                  <div>
                    <p className="text-muted-foreground">Cluster ID</p>
                    <p className="font-mono">{cluster.id}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">PostgreSQL Version</p>
                    <p>{cluster.version}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Tier</p>
                    <p className="capitalize">{cluster.config.tier}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Nodes</p>
                    <p>{cluster.config.nodeCount}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Storage</p>
                    <p>{cluster.config.storageGb} GB</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">High Availability</p>
                    <p>{cluster.config.highAvailability ? "Enabled" : "Disabled"}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Backups</p>
                    <p>{cluster.config.backupEnabled ? "Enabled" : "Disabled"}</p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Created</p>
                    <p>{formatDate(cluster.createdAt)}</p>
                  </div>
                </div>
              </CardContent>
            </Card>

            <Card>
              <CardHeader>
                <CardTitle>Quick Actions</CardTitle>
              </CardHeader>
              <CardContent className="space-y-3">
                <Button
                  variant="outline"
                  className="w-full justify-start"
                  onClick={handleCopyConnectionString}
                >
                  <Copy className="mr-2 h-4 w-4" />
                  Copy Connection String
                </Button>
                <Button
                  variant="outline"
                  className="w-full justify-start"
                  asChild
                >
                  <Link to="/clusters/$id/settings" params={{ id: clusterId }}>
                    <Settings className="mr-2 h-4 w-4" />
                    Cluster Settings
                  </Link>
                </Button>
                <Button
                  variant="outline"
                  className="w-full justify-start text-destructive hover:text-destructive"
                  onClick={() => setShowDeleteDialog(true)}
                >
                  <Trash2 className="mr-2 h-4 w-4" />
                  Delete Cluster
                </Button>
              </CardContent>
            </Card>
          </div>

          {cluster.status === "running" && metrics && (
            <Card>
              <CardHeader>
                <CardTitle>Query Performance</CardTitle>
              </CardHeader>
              <CardContent>
                <div className="grid grid-cols-3 gap-4 text-center">
                  <div>
                    <p className="text-3xl font-bold text-primary">
                      {metrics.queries.queriesPerSecond.toFixed(1)}
                    </p>
                    <p className="text-sm text-muted-foreground">Queries/sec</p>
                  </div>
                  <div>
                    <p className="text-3xl font-bold text-primary">
                      {metrics.queries.avgLatencyMs.toFixed(1)}ms
                    </p>
                    <p className="text-sm text-muted-foreground">Avg Latency</p>
                  </div>
                  <div>
                    <p className="text-3xl font-bold text-primary">
                      {metrics.queries.slowQueries}
                    </p>
                    <p className="text-sm text-muted-foreground">Slow Queries</p>
                  </div>
                </div>
              </CardContent>
            </Card>
          )}

          {/* Connection Pooler Status */}
          {cluster.poolerEnabled && (
            <PoolerStatusCard clusterId={clusterId} compact />
          )}
        </TabsContent>

        <TabsContent value="metrics" className="space-y-4">
          <MetricsChart
            data={metricsHistory ?? undefined}
            isLoading={!metricsHistory && cluster.status === "running"}
            onPeriodChange={handlePeriodChange}
          />
          <div className="grid gap-4 md:grid-cols-2">
            <ConnectionsChart
              data={metricsHistory ?? undefined}
              isLoading={!metricsHistory && cluster.status === "running"}
            />
            <QueryPerformanceChart
              data={metricsHistory ?? undefined}
              isLoading={!metricsHistory && cluster.status === "running"}
            />
          </div>
        </TabsContent>

        <TabsContent value="connection" className="space-y-4">
          <Card>
            <CardHeader>
              <CardTitle>Connection Details</CardTitle>
              <CardDescription>
                Use these details to connect to your PostgreSQL cluster
              </CardDescription>
            </CardHeader>
            <CardContent className="space-y-4">
              <div className="space-y-2">
                <Label>Connection String</Label>
                <div className="flex gap-2">
                  <Input
                    value={cluster.connectionString}
                    readOnly
                    className="font-mono text-sm"
                  />
                  <Button
                    variant="outline"
                    size="icon"
                    onClick={handleCopyConnectionString}
                  >
                    <Copy className="h-4 w-4" />
                  </Button>
                </div>
              </div>

              <Separator />

              <div className="grid grid-cols-2 gap-4 text-sm">
                <div>
                  <p className="text-muted-foreground">Host</p>
                  <p className="font-mono">{cluster.endpoints.primary}</p>
                </div>
                <div>
                  <p className="text-muted-foreground">Port</p>
                  <p className="font-mono">5432</p>
                </div>
                <div>
                  <p className="text-muted-foreground">Database</p>
                  <p className="font-mono">orochi</p>
                </div>
                <div>
                  <p className="text-muted-foreground">SSL Mode</p>
                  <p className="font-mono">require</p>
                </div>
              </div>

              {cluster.endpoints.replica && (
                <>
                  <Separator />
                  <div>
                    <p className="text-muted-foreground text-sm mb-2">
                      Read Replica Endpoint
                    </p>
                    <p className="font-mono text-sm">{cluster.endpoints.replica}</p>
                  </div>
                </>
              )}
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle>Connection Examples</CardTitle>
            </CardHeader>
            <CardContent className="space-y-4">
              <div className="space-y-2">
                <Label>psql</Label>
                <pre className="bg-muted p-3 rounded-md text-sm overflow-x-auto">
                  psql "{cluster.connectionString}"
                </pre>
              </div>
              <div className="space-y-2">
                <Label>Node.js (pg)</Label>
                <pre className="bg-muted p-3 rounded-md text-sm overflow-x-auto">
{`const { Pool } = require('pg');
const pool = new Pool({
  connectionString: '${cluster.connectionString}'
});`}
                </pre>
              </div>
              <div className="space-y-2">
                <Label>Python (psycopg2)</Label>
                <pre className="bg-muted p-3 rounded-md text-sm overflow-x-auto">
{`import psycopg2
conn = psycopg2.connect('${cluster.connectionString}')`}
                </pre>
              </div>
            </CardContent>
          </Card>
        </TabsContent>
      </Tabs>

      {/* Delete Dialog */}
      <Dialog open={showDeleteDialog} onOpenChange={setShowDeleteDialog}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Delete Cluster</DialogTitle>
            <DialogDescription>
              This action cannot be undone. This will permanently delete the
              cluster and all associated data.
            </DialogDescription>
          </DialogHeader>
          <div className="space-y-4 py-4">
            <p className="text-sm">
              Please type <span className="font-bold">{cluster.name}</span> to
              confirm.
            </p>
            <Input
              value={deleteConfirmation}
              onChange={(e) => setDeleteConfirmation(e.target.value)}
              placeholder="Enter cluster name"
            />
          </div>
          <DialogFooter>
            <Button
              variant="outline"
              onClick={() => setShowDeleteDialog(false)}
            >
              Cancel
            </Button>
            <Button
              variant="destructive"
              onClick={handleDelete}
              disabled={deleteConfirmation !== cluster.name || isActionLoading}
            >
              Delete Cluster
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  );
}
