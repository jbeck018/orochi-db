import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Workflow, TrendingUp, AlertCircle, Pause, Play } from "lucide-react";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { MetricCard } from "@/components/orochi/metric-card";
import { StatusBadge } from "@/components/orochi/status-badge";
import { orochiApi } from "@/lib/api";
import { formatDate } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { PipelineMetrics } from "@/types";

export const Route = createFileRoute("/clusters/$id/pipelines")({
  component: PipelinesPage,
});

function PipelinesPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<PipelineMetrics | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getPipelineMetrics(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load pipeline metrics",
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

  const handleTogglePipeline = async (pipelineId: string, pause: boolean): Promise<void> => {
    try {
      if (pause) {
        await orochiApi.pausePipeline(clusterId, pipelineId);
      } else {
        await orochiApi.resumePipeline(clusterId, pipelineId);
      }
      toast({ title: pause ? "Pipeline paused" : "Pipeline resumed" });
      fetchData();
    } catch (error) {
      toast({
        title: "Error",
        description: `Failed to ${pause ? "pause" : "resume"} pipeline`,
        variant: "destructive",
      });
    }
  };

  if (isLoading) {
    return (
      <div className="space-y-6">
        <Skeleton className="h-10 w-64" />
        <div className="grid gap-4 md:grid-cols-4">
          {[1, 2, 3, 4].map((i) => (
            <Skeleton key={i} className="h-32" />
          ))}
        </div>
      </div>
    );
  }

  if (!data) {
    return (
      <div className="flex flex-col items-center justify-center py-20">
        <p className="text-muted-foreground">No pipeline data available</p>
      </div>
    );
  }

  const activePipelines = data.pipelines.filter((p) => p.status === "running");
  const errorPipelines = data.pipelines.filter((p) => p.status === "error");

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-3xl font-bold tracking-tight">Pipeline Monitoring</h1>
        <p className="text-muted-foreground">
          Real-time data ingestion from Kafka, S3, and filesystems
        </p>
      </div>

      <div className="grid gap-4 md:grid-cols-4">
        <MetricCard
          title="Active Pipelines"
          value={activePipelines.length}
          description={`${data.pipelines.length} total`}
          icon={Workflow}
        />
        <MetricCard
          title="Records Processed"
          value={data.totalRecordsProcessed.toLocaleString()}
          icon={TrendingUp}
        />
        <MetricCard
          title="Avg Throughput"
          value={`${data.avgThroughputRecordsPerSec.toFixed(0)}`}
          description="records/sec"
          icon={TrendingUp}
        />
        <MetricCard
          title="Errors"
          value={data.totalErrors}
          description={`${errorPipelines.length} failed pipelines`}
          icon={AlertCircle}
        />
      </div>

      <Card>
        <CardHeader>
          <CardTitle>Active Pipelines</CardTitle>
          <CardDescription>
            Real-time data ingestion pipelines
          </CardDescription>
        </CardHeader>
        <CardContent>
          <div className="space-y-4">
            {data.pipelines.map((pipeline) => (
              <Card key={pipeline.pipelineId}>
                <CardContent className="pt-6">
                  <div className="flex items-start justify-between mb-4">
                    <div>
                      <h3 className="font-semibold">{pipeline.name}</h3>
                      <p className="text-sm text-muted-foreground">
                        {pipeline.source.type.toUpperCase()} → {pipeline.targetTable}
                      </p>
                    </div>
                    <div className="flex items-center gap-2">
                      <StatusBadge
                        status={
                          pipeline.status === "running"
                            ? "running"
                            : pipeline.status === "paused"
                            ? "paused"
                            : pipeline.status === "error"
                            ? "error"
                            : pipeline.status === "stopped"
                            ? "stopped"
                            : "unknown"
                        }
                      />
                      <Button
                        variant="outline"
                        size="sm"
                        onClick={() =>
                          handleTogglePipeline(
                            pipeline.pipelineId,
                            pipeline.status === "running"
                          )
                        }
                      >
                        {pipeline.status === "running" ? (
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
                      <p className="text-muted-foreground">Records</p>
                      <p className="font-medium">
                        {pipeline.metrics.recordsProcessed.toLocaleString()}
                      </p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Throughput</p>
                      <p className="font-medium">
                        {pipeline.metrics.throughputRecordsPerSec.toFixed(0)}/s
                      </p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Errors</p>
                      <p className="font-medium">{pipeline.metrics.errorCount}</p>
                    </div>
                    {pipeline.metrics.lagMs && (
                      <div>
                        <p className="text-muted-foreground">Lag</p>
                        <p className="font-medium">{pipeline.metrics.lagMs}ms</p>
                      </div>
                    )}
                    <div>
                      <p className="text-muted-foreground">Last Processed</p>
                      <p className="text-xs">
                        {pipeline.lastProcessed ? formatDate(pipeline.lastProcessed) : "N/A"}
                      </p>
                    </div>
                  </div>
                </CardContent>
              </Card>
            ))}
          </div>
        </CardContent>
      </Card>
    </div>
  );
}
