import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Columns, HardDrive, Percent, Database } from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { MetricCard } from "@/components/orochi/metric-card";
import { orochiApi } from "@/lib/api";
import { formatBytes, formatDate } from "@/lib/utils";
import { useToast } from "@/hooks/use-toast";
import type { ColumnarStats, CompressionType } from "@/types";
import { Cell, Pie, PieChart, ResponsiveContainer, Tooltip, Legend } from "recharts";

export const Route = createFileRoute("/clusters/$id/columnar")({
  component: ColumnarPage,
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

function ColumnarPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const { toast } = useToast();

  const [data, setData] = React.useState<ColumnarStats | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const fetchData = React.useCallback(async (): Promise<void> => {
    try {
      const response = await orochiApi.getColumnarStats(clusterId);
      setData(response.data);
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load columnar storage statistics",
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
          <p className="text-muted-foreground">No columnar storage data available</p>
        </div>
      </DashboardLayout>
    );
  }

  const compressionChartData = Object.entries(data.compressionByType).map(
    ([type, stats]) => ({
      name: type.toUpperCase(),
      value: stats.sizeBytes,
      count: stats.count,
      color: compressionColors[type as CompressionType],
    })
  );

  const storageSavings =
    ((data.totalUncompressedSize - data.totalCompressedSize) / data.totalUncompressedSize) * 100;

  return (
    <DashboardLayout>
      <div className="space-y-6">
        <div>
          <h1 className="text-3xl font-bold tracking-tight">Columnar Storage</h1>
          <p className="text-muted-foreground">
            Column-oriented format with compression for analytics
          </p>
        </div>

        <div className="grid gap-4 md:grid-cols-4">
          <MetricCard
            title="Columnar Tables"
            value={data.totalTables}
            icon={Database}
          />
          <MetricCard
            title="Total Stripes"
            value={data.totalStripes}
            icon={Columns}
          />
          <MetricCard
            title="Storage Savings"
            value={`${storageSavings.toFixed(1)}%`}
            description={`${data.avgCompressionRatio.toFixed(2)}x compression`}
            icon={Percent}
          />
          <MetricCard
            title="Compressed Size"
            value={formatBytes(data.totalCompressedSize)}
            description={`${formatBytes(data.totalUncompressedSize)} original`}
            icon={HardDrive}
          />
        </div>

        <div className="grid gap-4 md:grid-cols-2">
          <Card>
            <CardHeader>
              <CardTitle>Compression Type Distribution</CardTitle>
              <CardDescription>
                Breakdown by compression algorithm
              </CardDescription>
            </CardHeader>
            <CardContent>
              <ResponsiveContainer width="100%" height={300}>
                <PieChart>
                  <Pie
                    data={compressionChartData}
                    dataKey="value"
                    nameKey="name"
                    cx="50%"
                    cy="50%"
                    outerRadius={100}
                    label={(entry) => `${entry.name}: ${formatBytes(entry.value)}`}
                  >
                    {compressionChartData.map((entry, index) => (
                      <Cell key={`cell-${index}`} fill={entry.color} />
                    ))}
                  </Pie>
                  <Tooltip
                    formatter={(value: number) => formatBytes(value)}
                  />
                  <Legend />
                </PieChart>
              </ResponsiveContainer>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle>Compression Statistics</CardTitle>
              <CardDescription>
                Detailed breakdown by type
              </CardDescription>
            </CardHeader>
            <CardContent>
              <div className="space-y-4">
                {compressionChartData.map((item) => (
                  <div key={item.name} className="space-y-2">
                    <div className="flex items-center justify-between">
                      <div className="flex items-center gap-2">
                        <div
                          className="h-4 w-4 rounded"
                          style={{ backgroundColor: item.color }}
                        />
                        <span className="font-medium">{item.name}</span>
                      </div>
                      <span className="text-sm text-muted-foreground">
                        {item.count} columns
                      </span>
                    </div>
                    <div className="pl-6">
                      <p className="text-sm text-muted-foreground">
                        Size: {formatBytes(item.value)}
                      </p>
                    </div>
                  </div>
                ))}
              </div>
            </CardContent>
          </Card>
        </div>

        <Card>
          <CardHeader>
            <CardTitle>Storage Overview</CardTitle>
            <CardDescription>
              Total storage statistics across all columnar tables
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="grid grid-cols-3 gap-6 text-center">
              <div>
                <p className="text-3xl font-bold text-primary">
                  {data.totalRows.toLocaleString()}
                </p>
                <p className="text-sm text-muted-foreground mt-1">Total Rows</p>
              </div>
              <div>
                <p className="text-3xl font-bold text-primary">
                  {formatBytes(data.totalCompressedSize)}
                </p>
                <p className="text-sm text-muted-foreground mt-1">Compressed Size</p>
              </div>
              <div>
                <p className="text-3xl font-bold text-primary">
                  {data.avgCompressionRatio.toFixed(2)}x
                </p>
                <p className="text-sm text-muted-foreground mt-1">Avg Compression Ratio</p>
              </div>
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Recent Stripes</CardTitle>
            <CardDescription>
              Latest columnar data stripes
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="space-y-6">
              {data.recentStripes.map((stripe) => (
                <div
                  key={stripe.stripeId}
                  className="space-y-4 pb-6 last:pb-0 border-b last:border-0"
                >
                  <div className="flex items-start justify-between">
                    <div>
                      <h3 className="font-semibold">{stripe.tableName}</h3>
                      <p className="text-sm text-muted-foreground font-mono">
                        {stripe.stripeId}
                      </p>
                    </div>
                    <span className="text-sm text-muted-foreground">
                      {formatDate(stripe.createdAt)}
                    </span>
                  </div>

                  <div className="grid grid-cols-4 gap-4 text-sm">
                    <div>
                      <p className="text-muted-foreground">Rows</p>
                      <p className="font-medium">{stripe.rowCount.toLocaleString()}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Columns</p>
                      <p className="font-medium">{stripe.columnChunks.length}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Compressed Size</p>
                      <p className="font-medium">{formatBytes(stripe.totalCompressedSize)}</p>
                    </div>
                    <div>
                      <p className="text-muted-foreground">Ratio</p>
                      <p className="font-medium">{stripe.compressionRatio.toFixed(2)}x</p>
                    </div>
                  </div>

                  <div className="rounded-md border">
                    <table className="w-full text-sm">
                      <thead>
                        <tr className="border-b bg-muted/50">
                          <th className="p-2 text-left font-medium">Column</th>
                          <th className="p-2 text-left font-medium">Type</th>
                          <th className="p-2 text-left font-medium">Rows</th>
                          <th className="p-2 text-left font-medium">Nulls</th>
                          <th className="p-2 text-left font-medium">Size</th>
                          <th className="p-2 text-left font-medium">Compression</th>
                          <th className="p-2 text-left font-medium">Min/Max</th>
                        </tr>
                      </thead>
                      <tbody>
                        {stripe.columnChunks.map((column) => (
                          <tr key={column.columnName} className="border-b last:border-0">
                            <td className="p-2 font-mono text-xs">{column.columnName}</td>
                            <td className="p-2 text-xs">{column.dataType}</td>
                            <td className="p-2">{column.rowCount.toLocaleString()}</td>
                            <td className="p-2 text-muted-foreground">{column.nullCount}</td>
                            <td className="p-2">{formatBytes(column.compressedSizeBytes)}</td>
                            <td className="p-2">
                              <span className="px-2 py-0.5 text-xs rounded bg-muted font-mono">
                                {column.compressionType.toUpperCase()}
                              </span>
                            </td>
                            <td className="p-2 text-xs text-muted-foreground">
                              {column.minValue !== undefined && column.maxValue !== undefined
                                ? `${column.minValue} - ${column.maxValue}`
                                : "N/A"}
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </div>
              ))}
            </div>
          </CardContent>
        </Card>
      </div>
    </DashboardLayout>
  );
}
