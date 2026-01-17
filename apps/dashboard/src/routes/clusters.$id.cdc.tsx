import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Radio, TrendingUp, Clock, Pause, Play } from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { MetricCard } from "@/components/orochi/metric-card";
import { StatusBadge } from "@/components/orochi/status-badge";
import { orochiApi } from "@/lib/api";
import { formatBytes } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { CDCMetrics } from "@/types";

export const Route = createFileRoute("/clusters/$id/cdc")({
  component: CDCPage,
});

function CDCPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<CDCMetrics | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getCDCMetrics(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load CDC metrics",
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

  const handleToggleCDC = async (subscriptionId: string, pause: boolean): Promise<void> => {
    try {
      if (pause) {
        await orochiApi.pauseCDCSubscription(clusterId, subscriptionId);
      } else {
        await orochiApi.resumeCDCSubscription(clusterId, subscriptionId);
      }
      toast({ title: pause ? "Subscription paused" : "Subscription resumed" });
      fetchData();
    } catch (error) {
      toast({
        title: "Error",
        description: `Failed to ${pause ? "pause" : "resume"} subscription`,
        variant: "destructive",
      });
    }
  };

  if (isLoading) {
    return (
      <DashboardLayout>
        <div className="space-y-6">
          <Skeleton className="h-10 w-64" />
          <div className="grid gap-4 md:grid-cols-4">
            {[1, 2, 3, 4].map((i) => (
              <Skeleton key={i} className="h-32" />
            ))}
          </div>
        </div>
      </DashboardLayout>
    );
  }

  if (!data) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <p className="text-muted-foreground">No CDC data available</p>
        </div>
      </DashboardLayout>
    );
  }

  const activeSubscriptions = data.subscriptions.filter((s) => s.status === "active");

  return (
    <DashboardLayout>
      <div className="space-y-6">
        <div>
          <h1 className="text-3xl font-bold tracking-tight">Change Data Capture</h1>
          <p className="text-muted-foreground">
            Stream database changes to external systems
          </p>
        </div>

        <div className="grid gap-4 md:grid-cols-4">
          <MetricCard
            title="Active Subscriptions"
            value={activeSubscriptions.length}
            description={`${data.subscriptions.length} total`}
            icon={Radio}
          />
          <MetricCard
            title="Events Published"
            value={data.totalEventsPublished.toLocaleString()}
            icon={TrendingUp}
          />
          <MetricCard
            title="Data Streamed"
            value={formatBytes(data.totalBytesPublished)}
            icon={TrendingUp}
          />
          <MetricCard
            title="Avg Lag"
            value={`${data.avgLagMs.toFixed(0)}ms`}
            icon={Clock}
          />
        </div>

        <Card>
          <CardHeader>
            <CardTitle>Event Type Distribution</CardTitle>
            <CardDescription>
              Breakdown of CDC events by type
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="grid grid-cols-2 md:grid-cols-4 gap-4 text-center">
              {Object.entries(data.eventsByType).map(([type, count]) => (
                <div key={type}>
                  <p className="text-2xl font-bold text-primary">{count.toLocaleString()}</p>
                  <p className="text-sm text-muted-foreground capitalize">{type}</p>
                </div>
              ))}
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>CDC Subscriptions</CardTitle>
            <CardDescription>
              Active change data capture subscriptions
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="space-y-4">
              {data.subscriptions.map((subscription) => (
                <Card key={subscription.subscriptionId}>
                  <CardContent className="pt-6">
                    <div className="flex items-start justify-between mb-4">
                      <div>
                        <h3 className="font-semibold">{subscription.name}</h3>
                        <p className="text-sm text-muted-foreground">
                          {subscription.sourceTables.join(", ")} → {subscription.destination.type.toUpperCase()}
                        </p>
                      </div>
                      <div className="flex items-center gap-2">
                        <StatusBadge
                          status={
                            subscription.status === "active"
                              ? "active"
                              : subscription.status === "paused"
                              ? "paused"
                              : subscription.status === "error"
                              ? "error"
                              : "unknown"
                          }
                        />
                        <Button
                          variant="outline"
                          size="sm"
                          onClick={() =>
                            handleToggleCDC(
                              subscription.subscriptionId,
                              subscription.status === "active"
                            )
                          }
                        >
                          {subscription.status === "active" ? (
                            <>
                              <Pause className="mr-2 h-3 w-3" />
                              Pause
                            </>
                          ) : (
                            <>
                              <Play className="mr-2 h-3 w-3" />
                              Resume
                            </>
                          )}
                        </Button>
                      </div>
                    </div>

                    <div className="grid grid-cols-2 md:grid-cols-5 gap-4 text-sm">
                      <div>
                        <p className="text-muted-foreground">Events</p>
                        <p className="font-medium">
                          {subscription.metrics.eventsPublished.toLocaleString()}
                        </p>
                      </div>
                      <div>
                        <p className="text-muted-foreground">Throughput</p>
                        <p className="font-medium">
                          {subscription.metrics.throughputEventsPerSec.toFixed(0)}/s
                        </p>
                      </div>
                      <div>
                        <p className="text-muted-foreground">Data</p>
                        <p className="font-medium">
                          {formatBytes(subscription.metrics.bytesPublished)}
                        </p>
                      </div>
                      <div>
                        <p className="text-muted-foreground">Lag</p>
                        <p className="font-medium">{subscription.metrics.lagMs}ms</p>
                      </div>
                      <div>
                        <p className="text-muted-foreground">Errors</p>
                        <p className="font-medium">{subscription.metrics.errorCount}</p>
                      </div>
                    </div>

                    <div className="mt-4 flex flex-wrap gap-2">
                      {subscription.sourceTables.map((table) => (
                        <span
                          key={table}
                          className="px-2 py-1 text-xs rounded bg-muted font-mono"
                        >
                          {table}
                        </span>
                      ))}
                    </div>
                  </CardContent>
                </Card>
              ))}
            </div>
          </CardContent>
        </Card>
      </div>
    </DashboardLayout>
  );
}
