import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Database, RefreshCw, Activity } from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import { Skeleton } from "@/components/ui/skeleton";
import { MetricCard } from "@/components/orochi/metric-card";
import { StatusBadge } from "@/components/orochi/status-badge";
import { orochiApi } from "@/lib/api";
import { formatBytes, formatDate } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { ShardDistribution } from "@/types";

export const Route = createFileRoute("/clusters/$id/sharding")({
  component: ShardingPage,
});

function ShardingPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<ShardDistribution | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);
  const [isRebalancing, setIsRebalancing] = React.useState(false);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getShardDistribution(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load shard distribution",
        variant: "destructive",
      });
    } finally {
      setIsLoading(false);
    }
  }, [clusterId, toast]);

  React.useEffect(() => {
    fetchData();
    const interval = setInterval(fetchData, 30000);
    return () => clearInterval(interval);
  }, [fetchData]);

  const handleRebalance = async (): Promise<void> => {
    setIsRebalancing(true);
    try {
      await orochiApi.triggerRebalance(clusterId);
      toast({ title: "Rebalancing started" });
      fetchData();
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to trigger rebalancing",
        variant: "destructive",
      });
    } finally {
      setIsRebalancing(false);
    }
  };

  if (isLoading) {
    return (
      <DashboardLayout>
        <div className="space-y-6">
          <Skeleton className="h-10 w-64" />
          <div className="grid gap-4 md:grid-cols-3">
            {[1, 2, 3].map((i) => (
              <Skeleton key={i} className="h-32" />
            ))}
          </div>
          <Skeleton className="h-[400px]" />
        </div>
      </DashboardLayout>
    );
  }

  if (!data) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <p className="text-muted-foreground">No sharding data available</p>
        </div>
      </DashboardLayout>
    );
  }

  return (
    <DashboardLayout>
      <div className="space-y-6">
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-3xl font-bold tracking-tight">Shard Distribution</h1>
            <p className="text-muted-foreground">
              Hash-based horizontal distribution across nodes
            </p>
          </div>
          <Button
            onClick={handleRebalance}
            disabled={isRebalancing || data.rebalanceStatus.inProgress}
          >
            <RefreshCw className="mr-2 h-4 w-4" />
            Trigger Rebalance
          </Button>
        </div>

        <div className="grid gap-4 md:grid-cols-3">
          <MetricCard
            title="Total Shards"
            value={data.totalShards}
            icon={Database}
          />
          <MetricCard
            title="Hash Algorithm"
            value={data.hashAlgorithm}
            icon={Activity}
          />
          <MetricCard
            title="Rebalance Status"
            value={data.rebalanceStatus.inProgress ? "In Progress" : "Idle"}
            description={
              data.rebalanceStatus.inProgress
                ? `${data.rebalanceStatus.progress}% complete`
                : undefined
            }
          />
        </div>

        {data.rebalanceStatus.inProgress && (
          <Card>
            <CardHeader>
              <CardTitle>Rebalancing Progress</CardTitle>
              <CardDescription>
                Started: {formatDate(data.rebalanceStatus.startedAt ?? "")}
                {data.rebalanceStatus.estimatedCompletion && (
                  <> • ETA: {formatDate(data.rebalanceStatus.estimatedCompletion)}</>
                )}
              </CardDescription>
            </CardHeader>
            <CardContent>
              <Progress value={data.rebalanceStatus.progress} className="h-3" />
            </CardContent>
          </Card>
        )}

        <Card>
          <CardHeader>
            <CardTitle>Node Distribution</CardTitle>
            <CardDescription>
              Distribution of shards across cluster nodes
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="space-y-4">
              {data.nodeDistribution.map((node) => (
                <div key={node.nodeId} className="space-y-2">
                  <div className="flex items-center justify-between">
                    <div>
                      <p className="font-medium">{node.nodeName}</p>
                      <p className="text-sm text-muted-foreground">
                        {node.shardCount} shards • {formatBytes(node.totalSizeBytes)}
                      </p>
                    </div>
                    <span className="text-sm font-medium">
                      {node.totalRows.toLocaleString()} rows
                    </span>
                  </div>
                  <Progress
                    value={(node.shardCount / data.totalShards) * 100}
                    className="h-2"
                  />
                </div>
              ))}
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Shard Details</CardTitle>
            <CardDescription>
              Individual shard information and status
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="rounded-md border">
              <table className="w-full text-sm">
                <thead>
                  <tr className="border-b bg-muted/50">
                    <th className="p-3 text-left font-medium">Shard ID</th>
                    <th className="p-3 text-left font-medium">Node</th>
                    <th className="p-3 text-left font-medium">Rows</th>
                    <th className="p-3 text-left font-medium">Size</th>
                    <th className="p-3 text-left font-medium">Replication</th>
                    <th className="p-3 text-left font-medium">Status</th>
                    <th className="p-3 text-left font-medium">Last Modified</th>
                  </tr>
                </thead>
                <tbody>
                  {data.shards.map((shard) => {
                    const node = data.nodeDistribution.find(
                      (n) => n.nodeId === shard.nodeId
                    );
                    return (
                      <tr key={shard.shardId} className="border-b last:border-0">
                        <td className="p-3 font-mono">{shard.shardId}</td>
                        <td className="p-3">{node?.nodeName ?? shard.nodeId}</td>
                        <td className="p-3">{shard.rowCount.toLocaleString()}</td>
                        <td className="p-3">{formatBytes(shard.sizeBytes)}</td>
                        <td className="p-3">{shard.replicationFactor}x</td>
                        <td className="p-3">
                          <StatusBadge
                            status={
                              shard.status === "active"
                                ? "healthy"
                                : shard.status === "rebalancing"
                                ? "degraded"
                                : "unhealthy"
                            }
                          />
                        </td>
                        <td className="p-3 text-muted-foreground">
                          {formatDate(shard.lastModified)}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Shard Distribution Visualization</CardTitle>
            <CardDescription>
              Visual representation of shard placement across nodes
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="grid gap-2" style={{
              gridTemplateColumns: `repeat(${Math.ceil(Math.sqrt(data.totalShards))}, 1fr)`
            }}>
              {data.shards.map((shard) => {
                const node = data.nodeDistribution.find(
                  (n) => n.nodeId === shard.nodeId
                );
                const nodeIndex = data.nodeDistribution.indexOf(node!);
                const colors = [
                  "bg-blue-500",
                  "bg-green-500",
                  "bg-purple-500",
                  "bg-orange-500",
                  "bg-pink-500",
                  "bg-cyan-500",
                ];
                const color = colors[nodeIndex % colors.length];

                return (
                  <div
                    key={shard.shardId}
                    className={`h-12 rounded ${color} ${
                      shard.status === "rebalancing" ? "animate-pulse" : ""
                    } flex items-center justify-center text-white text-xs font-medium`}
                    title={`Shard ${shard.shardId} - ${node?.nodeName} - ${shard.status}`}
                  >
                    {shard.shardId}
                  </div>
                );
              })}
            </div>
            <div className="mt-4 flex flex-wrap gap-4">
              {data.nodeDistribution.map((node, index) => {
                const colors = [
                  "bg-blue-500",
                  "bg-green-500",
                  "bg-purple-500",
                  "bg-orange-500",
                  "bg-pink-500",
                  "bg-cyan-500",
                ];
                const color = colors[index % colors.length];

                return (
                  <div key={node.nodeId} className="flex items-center gap-2">
                    <div className={`h-4 w-4 rounded ${color}`} />
                    <span className="text-sm">{node.nodeName}</span>
                  </div>
                );
              })}
            </div>
          </CardContent>
        </Card>
      </div>
    </DashboardLayout>
  );
}
