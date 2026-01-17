import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import {Clock, TrendingUp, HardDrive, Zap } from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { MetricCard } from "@/components/orochi/metric-card";
import { TimeSeriesChart } from "@/components/orochi/time-series-chart";
import { StatusBadge } from "@/components/orochi/status-badge";
import { orochiApi } from "@/lib/api";
import { formatBytes, formatDate } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { TimeSeriesMetrics, CompressionType } from "@/types";

export const Route = createFileRoute("/clusters/$id/timeseries")({
  component: TimeSeriesPage,
});

const compressionColors: Record<CompressionType, string> = {
  none: "#94a3b8",
  lz4: "#3b82f6",
  zstd: "#8b5cf6",
  delta: "#10b981",
  gorilla: "#f59e0b",
  dictionary: "#ec4899",
  rle: "#06b6d4",
};

function TimeSeriesPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<TimeSeriesMetrics | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getTimeSeriesMetrics(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load time-series metrics",
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

  if (!data) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <p className="text-muted-foreground">No time-series data available</p>
        </div>
      </DashboardLayout>
    );
  }

  const compressionChartData = Object.entries(data.compressionStats.compressionByType).map(
    ([type, count]) => ({
      name: type.toUpperCase(),
      value: count,
      color: compressionColors[type as CompressionType],
    })
  );

  const chunkTimelineData = data.recentChunks
    .sort((a, b) => new Date(a.timeRange.start).getTime() - new Date(b.timeRange.start).getTime())
    .map((chunk) => ({
      timestamp: new Date(chunk.timeRange.start).toLocaleDateString(),
      rows: chunk.rowCount,
      compressedSize: chunk.compressedSizeBytes / 1024 / 1024,
      ratio: chunk.compressionRatio,
    }));

  return (
    <DashboardLayout>
      <div className="space-y-6">
        <div>
          <h1 className="text-3xl font-bold tracking-tight">Time-Series Metrics</h1>
          <p className="text-muted-foreground">
            Automatic time-based partitioning with chunks and compression
          </p>
        </div>

        <div className="grid gap-4 md:grid-cols-4">
          <MetricCard
            title="Hypertables"
            value={data.hypertables.length}
            icon={Clock}
          />
          <MetricCard
            title="Total Chunks"
            value={data.hypertables.reduce((sum, ht) => sum + ht.totalChunks, 0)}
            icon={HardDrive}
          />
          <MetricCard
            title="Avg Compression"
            value={`${data.compressionStats.avgCompressionRatio.toFixed(2)}x`}
            description="Space savings ratio"
            icon={Zap}
          />
          <MetricCard
            title="Ingestion Rate"
            value={`${data.ingestionRate.rowsPerSecond.toFixed(0)}`}
            description="rows/sec"
            icon={TrendingUp}
          />
        </div>

        <div className="grid gap-4 md:grid-cols-2">
          <TimeSeriesChart
            title="Chunk Row Count Timeline"
            description="Number of rows per chunk over time"
            data={chunkTimelineData}
            dataKeys={[
              { key: "rows", name: "Rows", color: "#3b82f6" },
            ]}
            type="bar"
            formatYAxis={(value) => `${(value / 1000).toFixed(0)}K`}
          />

          <TimeSeriesChart
            title="Chunk Compression Ratio"
            description="Compression effectiveness over time"
            data={chunkTimelineData}
            dataKeys={[
              { key: "ratio", name: "Ratio", color: "#10b981" },
            ]}
            formatYAxis={(value: number) => `${value.toFixed(2)}x`}
          />
        </div>

        <div className="grid gap-4 md:grid-cols-2">
          <Card>
            <CardHeader>
              <CardTitle>Compression Statistics</CardTitle>
              <CardDescription>
                Breakdown of compression types used
              </CardDescription>
            </CardHeader>
            <CardContent>
              <div className="space-y-4">
                <div className="grid grid-cols-2 gap-4 text-center">
                  <div>
                    <p className="text-2xl font-bold text-primary">
                      {formatBytes(data.compressionStats.totalCompressed)}
                    </p>
                    <p className="text-sm text-muted-foreground">Compressed</p>
                  </div>
                  <div>
                    <p className="text-2xl font-bold text-primary">
                      {formatBytes(data.compressionStats.totalUncompressed)}
                    </p>
                    <p className="text-sm text-muted-foreground">Uncompressed</p>
                  </div>
                </div>

                <div className="space-y-3">
                  {compressionChartData.map((item) => (
                    <div key={item.name} className="space-y-1">
                      <div className="flex items-center justify-between text-sm">
                        <span className="font-medium">{item.name}</span>
                        <span className="text-muted-foreground">{item.value} chunks</span>
                      </div>
                      <div className="h-2 rounded-full bg-muted">
                        <div
                          className="h-full rounded-full transition-all"
                          style={{
                            width: `${(item.value / data.recentChunks.length) * 100}%`,
                            backgroundColor: item.color,
                          }}
                        />
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle>Ingestion Metrics</CardTitle>
              <CardDescription>
                Real-time data ingestion statistics
              </CardDescription>
            </CardHeader>
            <CardContent>
              <div className="space-y-4">
                <div className="grid grid-cols-2 gap-4">
                  <div>
                    <p className="text-sm text-muted-foreground">Rows/sec</p>
                    <p className="text-2xl font-bold">
                      {data.ingestionRate.rowsPerSecond.toLocaleString()}
                    </p>
                  </div>
                  <div>
                    <p className="text-sm text-muted-foreground">Throughput</p>
                    <p className="text-2xl font-bold">
                      {formatBytes(data.ingestionRate.bytesPerSecond)}/s
                    </p>
                  </div>
                </div>
                <div>
                  <p className="text-sm text-muted-foreground">Last Updated</p>
                  <p className="font-medium">{formatDate(data.ingestionRate.timestamp)}</p>
                </div>
              </div>
            </CardContent>
          </Card>
        </div>

        <Card>
          <CardHeader>
            <CardTitle>Hypertables</CardTitle>
            <CardDescription>
              Time-series optimized tables configuration
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="space-y-6">
              {data.hypertables.map((hypertable) => (
                <div key={hypertable.tableName} className="space-y-4 pb-6 last:pb-0 border-b last:border-0">
                  <div className="flex items-start justify-between">
                    <div>
                      <h3 className="font-semibold">{hypertable.tableName}</h3>
                      <p className="text-sm text-muted-foreground">
                        Partitioned by {hypertable.partitionColumn} • {hypertable.chunkInterval} chunks
                      </p>
                    </div>
                    <StatusBadge
                      status={hypertable.compressionEnabled ? "healthy" : "unknown"}
                    />
                  </div>

                  <div className="grid grid-cols-4 gap-4 text-sm">
                    <div>
                      <p className="text-muted-foreground">Chunks</p>
                      <p className="font-medium">{hypertable.totalChunks}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Rows</p>
                      <p className="font-medium">{hypertable.totalRows.toLocaleString()}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Size</p>
                      <p className="font-medium">{formatBytes(hypertable.totalSizeBytes)}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Aggregates</p>
                      <p className="font-medium">{hypertable.continuousAggregates.length}</p>
                    </div>
                  </div>

                  {hypertable.continuousAggregates.length > 0 && (
                    <div className="flex flex-wrap gap-2">
                      {hypertable.continuousAggregates.map((agg) => (
                        <span
                          key={agg}
                          className="px-2 py-1 text-xs rounded-md bg-muted font-mono"
                        >
                          {agg}
                        </span>
                      ))}
                    </div>
                  )}
                </div>
              ))}
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Recent Chunks</CardTitle>
            <CardDescription>
              Latest time-series data chunks
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="rounded-md border">
              <table className="w-full text-sm">
                <thead>
                  <tr className="border-b bg-muted/50">
                    <th className="p-3 text-left font-medium">Chunk ID</th>
                    <th className="p-3 text-left font-medium">Table</th>
                    <th className="p-3 text-left font-medium">Time Range</th>
                    <th className="p-3 text-left font-medium">Rows</th>
                    <th className="p-3 text-left font-medium">Size</th>
                    <th className="p-3 text-left font-medium">Ratio</th>
                    <th className="p-3 text-left font-medium">Type</th>
                    <th className="p-3 text-left font-medium">Status</th>
                  </tr>
                </thead>
                <tbody>
                  {data.recentChunks.map((chunk) => (
                    <tr key={chunk.chunkId} className="border-b last:border-0">
                      <td className="p-3 font-mono text-xs">{chunk.chunkId.slice(0, 8)}...</td>
                      <td className="p-3">{chunk.tableName}</td>
                      <td className="p-3 text-xs">
                        {new Date(chunk.timeRange.start).toLocaleDateString()} -{" "}
                        {new Date(chunk.timeRange.end).toLocaleDateString()}
                      </td>
                      <td className="p-3">{chunk.rowCount.toLocaleString()}</td>
                      <td className="p-3">{formatBytes(chunk.compressedSizeBytes)}</td>
                      <td className="p-3 font-medium">{chunk.compressionRatio.toFixed(2)}x</td>
                      <td className="p-3">
                        <span className="px-2 py-1 text-xs rounded bg-muted font-mono">
                          {chunk.compressionType.toUpperCase()}
                        </span>
                      </td>
                      <td className="p-3">
                        <StatusBadge
                          status={
                            chunk.status === "active"
                              ? "healthy"
                              : chunk.status === "compressed"
                              ? "degraded"
                              : "unknown"
                          }
                        />
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </CardContent>
        </Card>
      </div>
    </DashboardLayout>
  );
}
