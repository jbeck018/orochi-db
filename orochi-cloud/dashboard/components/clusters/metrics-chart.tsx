"use client";

import * as React from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  Legend,
} from "recharts";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Skeleton } from "@/components/ui/skeleton";
import type { ClusterMetricsHistory } from "@/types";

// Constants to avoid recreating objects on each render
const CHART_MARGIN = { top: 5, right: 30, left: 20, bottom: 5 };
const TOOLTIP_CONTENT_STYLE = {
  backgroundColor: "hsl(var(--card))",
  border: "1px solid hsl(var(--border))",
  borderRadius: "var(--radius)",
};
const TOOLTIP_LABEL_STYLE = { color: "hsl(var(--foreground))" };

interface MetricsChartProps {
  data?: ClusterMetricsHistory;
  isLoading?: boolean;
  onPeriodChange?: (period: "1h" | "6h" | "24h" | "7d" | "30d") => void;
}

// Memoize MetricsChart to prevent unnecessary re-renders
export const MetricsChart = React.memo(function MetricsChart({
  data,
  isLoading,
  onPeriodChange,
}: MetricsChartProps): React.JSX.Element {
  const [period, setPeriod] = React.useState<"1h" | "6h" | "24h" | "7d" | "30d">("24h");

  const handlePeriodChange = React.useCallback((value: string): void => {
    const newPeriod = value as "1h" | "6h" | "24h" | "7d" | "30d";
    setPeriod(newPeriod);
    onPeriodChange?.(newPeriod);
  }, [onPeriodChange]);

  // Memoize chart data transformation to avoid recalculating on every render
  const chartData = React.useMemo(() => {
    if (!data?.dataPoints) return undefined;

    return data.dataPoints.map((point) => {
      const date = new Date(point.timestamp);
      let time: string;
      if (period === "1h" || period === "6h" || period === "24h") {
        time = date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
      } else {
        time = date.toLocaleDateString([], { month: "short", day: "numeric" });
      }
      return { ...point, time };
    });
  }, [data, period]);

  if (isLoading) {
    return (
      <Card>
        <CardHeader>
          <div className="flex items-center justify-between">
            <div className="space-y-2">
              <Skeleton className="h-6 w-32" />
              <Skeleton className="h-4 w-48" />
            </div>
            <Skeleton className="h-10 w-24" />
          </div>
        </CardHeader>
        <CardContent>
          <Skeleton className="h-[300px] w-full" />
        </CardContent>
      </Card>
    );
  }

  return (
    <Card>
      <CardHeader>
        <div className="flex items-center justify-between">
          <div>
            <CardTitle>Cluster Metrics</CardTitle>
            <CardDescription>
              Performance metrics over time
            </CardDescription>
          </div>
          <Select value={period} onValueChange={handlePeriodChange}>
            <SelectTrigger className="w-24">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="1h">1 hour</SelectItem>
              <SelectItem value="6h">6 hours</SelectItem>
              <SelectItem value="24h">24 hours</SelectItem>
              <SelectItem value="7d">7 days</SelectItem>
              <SelectItem value="30d">30 days</SelectItem>
            </SelectContent>
          </Select>
        </div>
      </CardHeader>
      <CardContent>
        {chartData && chartData.length > 0 ? (
          <div className="h-[300px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart
                data={chartData}
                margin={CHART_MARGIN}
              >
                <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                <XAxis
                  dataKey="time"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                  domain={[0, 100]}
                  tickFormatter={(value) => `${value}%`}
                />
                <Tooltip
                  contentStyle={TOOLTIP_CONTENT_STYLE}
                  labelStyle={TOOLTIP_LABEL_STYLE}
                />
                <Legend />
                <Line
                  type="monotone"
                  dataKey="cpuPercent"
                  name="CPU"
                  stroke="hsl(var(--primary))"
                  strokeWidth={2}
                  dot={false}
                />
                <Line
                  type="monotone"
                  dataKey="memoryPercent"
                  name="Memory"
                  stroke="#22c55e"
                  strokeWidth={2}
                  dot={false}
                />
                <Line
                  type="monotone"
                  dataKey="storagePercent"
                  name="Storage"
                  stroke="#eab308"
                  strokeWidth={2}
                  dot={false}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        ) : (
          <div className="flex h-[300px] items-center justify-center text-muted-foreground">
            No metrics data available
          </div>
        )}
      </CardContent>
    </Card>
  );
}

interface ConnectionsChartProps {
  data?: ClusterMetricsHistory;
  isLoading?: boolean;
}

// Memoize ConnectionsChart to prevent unnecessary re-renders
export const ConnectionsChart = React.memo(function ConnectionsChart({
  data,
  isLoading,
}: ConnectionsChartProps): React.JSX.Element {
  // Memoize chart data transformation
  const chartData = React.useMemo(() => {
    if (!data?.dataPoints) return undefined;
    return data.dataPoints.map((point) => ({
      time: new Date(point.timestamp).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
      }),
      connections: point.connections,
    }));
  }, [data]);

  if (isLoading) {
    return (
      <Card>
        <CardHeader>
          <Skeleton className="h-6 w-32" />
          <Skeleton className="h-4 w-48" />
        </CardHeader>
        <CardContent>
          <Skeleton className="h-[200px] w-full" />
        </CardContent>
      </Card>
    );
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle>Active Connections</CardTitle>
        <CardDescription>Database connection count over time</CardDescription>
      </CardHeader>
      <CardContent>
        {chartData && chartData.length > 0 ? (
          <div className="h-[200px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart
                data={chartData}
                margin={{ top: 5, right: 30, left: 20, bottom: 5 }}
              >
                <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                <XAxis
                  dataKey="time"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <Tooltip
                  contentStyle={TOOLTIP_CONTENT_STYLE}
                  labelStyle={TOOLTIP_LABEL_STYLE}
                />
                <Line
                  type="monotone"
                  dataKey="connections"
                  name="Connections"
                  stroke="hsl(var(--primary))"
                  strokeWidth={2}
                  dot={false}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        ) : (
          <div className="flex h-[200px] items-center justify-center text-muted-foreground">
            No connection data available
          </div>
        )}
      </CardContent>
    </Card>
  );
});

interface QueryPerformanceChartProps {
  data?: ClusterMetricsHistory;
  isLoading?: boolean;
}

// Memoize QueryPerformanceChart to prevent unnecessary re-renders
export const QueryPerformanceChart = React.memo(function QueryPerformanceChart({
  data,
  isLoading,
}: QueryPerformanceChartProps): React.JSX.Element {
  // Memoize chart data transformation
  const chartData = React.useMemo(() => {
    if (!data?.dataPoints) return undefined;
    return data.dataPoints.map((point) => ({
      time: new Date(point.timestamp).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
      }),
      qps: point.qps,
      latency: point.latencyMs,
    }));
  }, [data]);

  if (isLoading) {
    return (
      <Card>
        <CardHeader>
          <Skeleton className="h-6 w-32" />
          <Skeleton className="h-4 w-48" />
        </CardHeader>
        <CardContent>
          <Skeleton className="h-[200px] w-full" />
        </CardContent>
      </Card>
    );
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle>Query Performance</CardTitle>
        <CardDescription>Queries per second and latency</CardDescription>
      </CardHeader>
      <CardContent>
        {chartData && chartData.length > 0 ? (
          <div className="h-[200px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart
                data={chartData}
                margin={{ top: 5, right: 30, left: 20, bottom: 5 }}
              >
                <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                <XAxis
                  dataKey="time"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  yAxisId="left"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  yAxisId="right"
                  orientation="right"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                  tickFormatter={(value) => `${value}ms`}
                />
                <Tooltip
                  contentStyle={TOOLTIP_CONTENT_STYLE}
                  labelStyle={TOOLTIP_LABEL_STYLE}
                />
                <Legend />
                <Line
                  yAxisId="left"
                  type="monotone"
                  dataKey="qps"
                  name="QPS"
                  stroke="hsl(var(--primary))"
                  strokeWidth={2}
                  dot={false}
                />
                <Line
                  yAxisId="right"
                  type="monotone"
                  dataKey="latency"
                  name="Latency (ms)"
                  stroke="#f97316"
                  strokeWidth={2}
                  dot={false}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        ) : (
          <div className="flex h-[200px] items-center justify-center text-muted-foreground">
            No query data available
          </div>
        )}
      </CardContent>
    </Card>
  );
});
