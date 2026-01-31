import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Zap } from "lucide-react";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Skeleton } from "@/components/ui/skeleton";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { AlertCircle } from "lucide-react";
import { PoolerStatusCard } from "@/components/clusters/pooler-status-card";
import { PoolerStatsChart } from "@/components/clusters/pooler-stats-chart";
import { PoolerConfigPanel } from "@/components/clusters/pooler-config-panel";
import { PoolerClientsTable } from "@/components/clusters/pooler-clients-table";
import { usePoolerStatus } from "@/hooks/api";

export const Route = createFileRoute("/clusters/$id/pooler")({
  component: PoolerPage,
});

function PoolerPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const [activeTab, setActiveTab] = React.useState("overview");

  const { data: statusData, isLoading, error } = usePoolerStatus(clusterId);
  const status = statusData?.data;

  if (isLoading) {
    return (
      <div className="space-y-6">
        <div className="flex items-center gap-2">
          <Zap className="h-6 w-6 text-blue-500" />
          <Skeleton className="h-8 w-64" />
        </div>
        <Skeleton className="h-[200px]" />
        <Skeleton className="h-[400px]" />
      </div>
    );
  }

  if (error) {
    return (
      <div className="space-y-6">
        <div className="flex items-center gap-2">
          <Zap className="h-6 w-6 text-blue-500" />
          <h2 className="text-2xl font-bold">PgDog Connection Pooler</h2>
        </div>
        <Alert variant="destructive">
          <AlertCircle className="h-4 w-4" />
          <AlertTitle>Error Loading Pooler Status</AlertTitle>
          <AlertDescription>
            Unable to fetch pooler status. Please check that the pooler is configured
            for this cluster and try again.
          </AlertDescription>
        </Alert>
      </div>
    );
  }

  return (
    <div className="space-y-6">
      {/* Page Header */}
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Zap className="h-6 w-6 text-blue-500" />
          <div>
            <h2 className="text-2xl font-bold">PgDog Connection Pooler</h2>
            <p className="text-muted-foreground">
              Manage connection pooling, query routing, and client connections
            </p>
          </div>
        </div>
      </div>

      {/* Status Overview Card */}
      <PoolerStatusCard clusterId={clusterId} />

      {/* Tabbed Content */}
      <Tabs value={activeTab} onValueChange={setActiveTab} className="space-y-4">
        <TabsList>
          <TabsTrigger value="overview">Overview</TabsTrigger>
          <TabsTrigger value="stats">Statistics</TabsTrigger>
          <TabsTrigger value="clients">Clients</TabsTrigger>
          <TabsTrigger value="config">Configuration</TabsTrigger>
        </TabsList>

        {/* Overview Tab */}
        <TabsContent value="overview" className="space-y-4">
          <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-4">
            <StatCard
              title="Total Queries"
              value={status?.stats.totalQueries.toLocaleString() ?? "0"}
              description="Queries processed"
            />
            <StatCard
              title="Throughput"
              value={`${status?.stats.queriesPerSecond.toFixed(1) ?? "0"} QPS`}
              description="Current query rate"
            />
            <StatCard
              title="Avg Latency"
              value={`${status?.stats.avgLatencyMs.toFixed(2) ?? "0"}ms`}
              description="Average query latency"
            />
            <StatCard
              title="P99 Latency"
              value={`${status?.stats.p99LatencyMs.toFixed(2) ?? "0"}ms`}
              description="99th percentile"
            />
          </div>

          {/* Quick stats charts */}
          <PoolerStatsChart clusterId={clusterId} />
        </TabsContent>

        {/* Statistics Tab */}
        <TabsContent value="stats" className="space-y-4">
          <PoolerStatsChart clusterId={clusterId} />
        </TabsContent>

        {/* Clients Tab */}
        <TabsContent value="clients" className="space-y-4">
          <PoolerClientsTable clusterId={clusterId} />
        </TabsContent>

        {/* Configuration Tab */}
        <TabsContent value="config" className="space-y-4">
          <PoolerConfigPanel clusterId={clusterId} />
        </TabsContent>
      </Tabs>
    </div>
  );
}

// Helper component for stat cards
interface StatCardProps {
  title: string;
  value: string;
  description: string;
}

function StatCard({ title, value, description }: StatCardProps): React.JSX.Element {
  return (
    <div className="rounded-lg border bg-card p-4">
      <p className="text-sm text-muted-foreground">{title}</p>
      <p className="mt-1 text-2xl font-bold">{value}</p>
      <p className="text-xs text-muted-foreground">{description}</p>
    </div>
  );
}
