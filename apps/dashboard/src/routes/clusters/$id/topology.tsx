import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Server, Shield, HardDrive } from "lucide-react";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Progress } from "@/components/ui/progress";
import { Skeleton } from "@/components/ui/skeleton";
import { Badge } from "@/components/ui/badge";
import { MetricCard } from "@/components/orochi/metric-card";
import { StatusBadge, type HealthStatus } from "@/components/orochi/status-badge";
import { orochiApi } from "@/lib/api";
import { formatBytes, formatDate } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { ClusterTopology, StorageTier } from "@/types";

export const Route = createFileRoute("/clusters/$id/topology")({
  component: TopologyPage,
});

const tierColors: Record<StorageTier, string> = {
  hot: "#ef4444",
  warm: "#f59e0b",
  cold: "#3b82f6",
  frozen: "#6366f1",
};

function TopologyPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<ClusterTopology | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getTopology(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load cluster topology",
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

  if (isLoading) {
    return (
      <div className="space-y-6">
        <Skeleton className="h-10 w-64" />
        <div className="grid gap-4 md:grid-cols-3">
          {[1, 2, 3].map((i) => (
            <Skeleton key={i} className="h-32" />
          ))}
        </div>
        <Skeleton className="h-[400px]" />
      </div>
    );
  }

  if (!data) {
    return (
      <div className="flex flex-col items-center justify-center py-20">
        <p className="text-muted-foreground">No topology data available</p>
      </div>
    );
  }

  const healthyNodes = data.nodes.filter((n) => n.status === "healthy").length;
  const totalStorage = data.tierDistribution.reduce((sum, t) => sum + t.sizeBytes, 0);

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-3xl font-bold tracking-tight">Cluster Topology</h1>
        <p className="text-muted-foreground">
          Node health, Raft consensus, and storage tier distribution
        </p>
      </div>

      <div className="grid gap-4 md:grid-cols-3">
        <MetricCard
          title="Healthy Nodes"
          value={`${healthyNodes}/${data.nodes.length}`}
          icon={Server}
        />
        <MetricCard
          title="Consensus Status"
          value={data.consensus.healthy ? "Healthy" : "Degraded"}
          description={`Leader: ${data.consensus.leader}`}
          icon={Shield}
        />
        <MetricCard
          title="Total Storage"
          value={formatBytes(totalStorage)}
          description={`${data.tierDistribution.length} tiers`}
          icon={HardDrive}
        />
      </div>

      <Card>
        <CardHeader>
          <CardTitle>Raft Consensus Status</CardTitle>
          <CardDescription>
            Distributed consensus and leader election information
          </CardDescription>
        </CardHeader>
        <CardContent>
          <div className="space-y-4">
            <div className="grid grid-cols-4 gap-4 text-center">
              <div>
                <p className="text-sm text-muted-foreground">Current Leader</p>
                <p className="text-lg font-bold">{data.consensus.leader}</p>
              </div>
              <div>
                <p className="text-sm text-muted-foreground">Quorum Size</p>
                <p className="text-lg font-bold">{data.consensus.quorumSize}</p>
              </div>
              <div>
                <p className="text-sm text-muted-foreground">Last Election</p>
                <p className="text-lg font-bold">{formatDate(data.consensus.lastElection)}</p>
              </div>
              <div>
                <p className="text-sm text-muted-foreground">Status</p>
                <StatusBadge
                  status={data.consensus.healthy ? "healthy" : "degraded"}
                />
              </div>
            </div>

            <div className="rounded-md border">
              <table className="w-full text-sm">
                <thead>
                  <tr className="border-b bg-muted/50">
                    <th className="p-3 text-left font-medium">Node</th>
                    <th className="p-3 text-left font-medium">State</th>
                    <th className="p-3 text-left font-medium">Term</th>
                    <th className="p-3 text-left font-medium">Commit Index</th>
                    <th className="p-3 text-left font-medium">Last Applied</th>
                    <th className="p-3 text-left font-medium">Followers</th>
                  </tr>
                </thead>
                <tbody>
                  {data.raftStatus.map((raft) => (
                    <tr key={raft.nodeId} className="border-b last:border-0">
                      <td className="p-3 font-mono">{raft.nodeId}</td>
                      <td className="p-3">
                        <Badge
                          variant={
                            raft.state === "leader"
                              ? "default"
                              : raft.state === "follower"
                              ? "secondary"
                              : "outline"
                          }
                        >
                          {raft.state.toUpperCase()}
                        </Badge>
                      </td>
                      <td className="p-3">{raft.term}</td>
                      <td className="p-3">{raft.commitIndex}</td>
                      <td className="p-3">{raft.lastApplied}</td>
                      <td className="p-3">
                        {raft.state === "leader"
                          ? raft.followers.length
                          : "-"}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Node Health Status</CardTitle>
          <CardDescription>
            Real-time health metrics for each cluster node
          </CardDescription>
        </CardHeader>
        <CardContent>
          <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-3">
            {data.nodes.map((node) => (
              <Card key={node.nodeId}>
                <CardHeader className="pb-3">
                  <div className="flex items-center justify-between">
                    <CardTitle className="text-base">{node.nodeName}</CardTitle>
                    <StatusBadge status={node.status as HealthStatus} />
                  </div>
                  <CardDescription className="text-xs font-mono">
                    {node.nodeId}
                  </CardDescription>
                </CardHeader>
                <CardContent className="space-y-4">
                  <div>
                    <div className="flex items-center justify-between text-sm mb-1">
                      <span className="text-muted-foreground">CPU</span>
                      <span className="font-medium">
                        {node.cpu.usagePercent.toFixed(1)}%
                      </span>
                    </div>
                    <Progress value={node.cpu.usagePercent} className="h-2" />
                    <p className="text-xs text-muted-foreground mt-1">
                      {node.cpu.cores} cores
                    </p>
                  </div>

                  <div>
                    <div className="flex items-center justify-between text-sm mb-1">
                      <span className="text-muted-foreground">Memory</span>
                      <span className="font-medium">
                        {node.memory.usagePercent.toFixed(1)}%
                      </span>
                    </div>
                    <Progress value={node.memory.usagePercent} className="h-2" />
                    <p className="text-xs text-muted-foreground mt-1">
                      {formatBytes(node.memory.usedBytes)} / {formatBytes(node.memory.totalBytes)}
                    </p>
                  </div>

                  <div>
                    <div className="flex items-center justify-between text-sm mb-1">
                      <span className="text-muted-foreground">Disk</span>
                      <span className="font-medium">
                        {node.disk.usagePercent.toFixed(1)}%
                      </span>
                    </div>
                    <Progress value={node.disk.usagePercent} className="h-2" />
                    <p className="text-xs text-muted-foreground mt-1">
                      {formatBytes(node.disk.usedBytes)} / {formatBytes(node.disk.totalBytes)}
                    </p>
                  </div>

                  <div className="pt-2 border-t">
                    <div className="flex items-center justify-between text-sm">
                      <span className="text-muted-foreground">Role</span>
                      <Badge variant="outline">{node.role.toUpperCase()}</Badge>
                    </div>
                    <div className="flex items-center justify-between text-sm mt-2">
                      <span className="text-muted-foreground">Connections</span>
                      <span className="font-medium">{node.connections}</span>
                    </div>
                    <div className="flex items-center justify-between text-sm mt-2">
                      <span className="text-muted-foreground">Last Heartbeat</span>
                      <span className="text-xs">{formatDate(node.lastHeartbeat)}</span>
                    </div>
                  </div>
                </CardContent>
              </Card>
            ))}
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Storage Tier Distribution</CardTitle>
          <CardDescription>
            Hot/warm/cold/frozen data lifecycle distribution
          </CardDescription>
        </CardHeader>
        <CardContent>
          <div className="space-y-4">
            {data.tierDistribution.map((tier) => (
              <div key={tier.tier} className="space-y-2">
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-3">
                    <div
                      className="h-4 w-4 rounded"
                      style={{ backgroundColor: tierColors[tier.tier] }}
                    />
                    <div>
                      <p className="font-medium capitalize">{tier.tier} Tier</p>
                      <p className="text-xs text-muted-foreground">
                        Age threshold: {tier.ageThreshold}
                      </p>
                    </div>
                  </div>
                  <div className="text-right">
                    <p className="font-medium">{formatBytes(tier.sizeBytes)}</p>
                    <p className="text-xs text-muted-foreground">
                      {tier.rowCount.toLocaleString()} rows
                    </p>
                  </div>
                </div>
                <Progress
                  value={(tier.sizeBytes / totalStorage) * 100}
                  className="h-2"
                  style={{
                    backgroundColor: `${tierColors[tier.tier]}20`,
                  }}
                />
                <p className="text-xs text-muted-foreground">
                  {tier.tableCount} tables • {((tier.sizeBytes / totalStorage) * 100).toFixed(1)}%
                  of total storage
                </p>
              </div>
            ))}
          </div>
        </CardContent>
      </Card>
    </div>
  );
}
