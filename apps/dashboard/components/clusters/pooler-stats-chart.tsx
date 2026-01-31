"use client";

import * as React from "react";
import {
  LineChart,
  Line,
  AreaChart,
  Area,
  BarChart,
  Bar,
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
import { usePoolerStatsHistory } from "@/hooks/api";

// Chart styling constants
const CHART_MARGIN = { top: 5, right: 30, left: 20, bottom: 5 };
const TOOLTIP_CONTENT_STYLE = {
  backgroundColor: "hsl(var(--card))",
  border: "1px solid hsl(var(--border))",
  borderRadius: "var(--radius)",
};
const TOOLTIP_LABEL_STYLE = { color: "hsl(var(--foreground))" };

interface PoolerStatsChartProps {
  clusterId: string;
}

export const PoolerStatsChart = React.memo(function PoolerStatsChart({
  clusterId,
}: PoolerStatsChartProps): React.JSX.Element {
  const [period, setPeriod] = React.useState<"1h" | "6h" | "24h" | "7d">("1h");

  const { data: historyData, isLoading } = usePoolerStatsHistory(clusterId, period);
  const history = historyData?.data;

  // Transform data for charts
  const chartData = React.useMemo(() => {
    if (!history?.dataPoints) return [];

    return history.dataPoints.map((point) => {
      const date = new Date(point.timestamp);
      let time: string;
      if (period === "1h" || period === "6h") {
        time = date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
      } else {
        time = date.toLocaleDateString([], { month: "short", day: "numeric", hour: "2-digit" });
      }
      return {
        ...point,
        time,
      };
    });
  }, [history, period]);

  const handlePeriodChange = React.useCallback((value: string): void => {
    setPeriod(value as "1h" | "6h" | "24h" | "7d");
  }, []);

  if (isLoading) {
    return (
      <div className="space-y-4">
        <Card>
          <CardHeader>
            <div className="flex items-center justify-between">
              <div className="space-y-2">
                <Skeleton className="h-6 w-40" />
                <Skeleton className="h-4 w-56" />
              </div>
              <Skeleton className="h-10 w-24" />
            </div>
          </CardHeader>
          <CardContent>
            <Skeleton className="h-[300px] w-full" />
          </CardContent>
        </Card>
      </div>
    );
  }

  return (
    <div className="space-y-4">
      {/* Pool Utilization Chart */}
      <Card>
        <CardHeader>
          <div className="flex items-center justify-between">
            <div>
              <CardTitle>Pool Utilization</CardTitle>
              <CardDescription>
                Connection pool usage over time
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
              </SelectContent>
            </Select>
          </div>
        </CardHeader>
        <CardContent>
          {chartData.length > 0 ? (
            <div className="h-[300px]">
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={chartData} margin={CHART_MARGIN}>
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
                    formatter={(value: number) => [`${value.toFixed(1)}%`, "Utilization"]}
                  />
                  <Area
                    type="monotone"
                    dataKey="poolUtilization"
                    name="Pool Utilization"
                    stroke="hsl(var(--primary))"
                    fill="hsl(var(--primary))"
                    fillOpacity={0.2}
                    strokeWidth={2}
                  />
                </AreaChart>
              </ResponsiveContainer>
            </div>
          ) : (
            <div className="flex h-[300px] items-center justify-center text-muted-foreground">
              No utilization data available
            </div>
          )}
        </CardContent>
      </Card>

      <div className="grid gap-4 md:grid-cols-2">
        {/* Connections Chart */}
        <ConnectionsChart data={chartData} />

        {/* Query Throughput Chart */}
        <QueryThroughputChart data={chartData} />
      </div>

      {/* Latency Chart */}
      <LatencyChart data={chartData} />
    </div>
  );
});

interface ChartDataPoint {
  time: string;
  activeConnections: number;
  idleConnections: number;
  waitingClients: number;
  queriesPerSecond: number;
  avgLatencyMs: number;
  poolUtilization: number;
}

interface SubChartProps {
  data: ChartDataPoint[];
}

const ConnectionsChart = React.memo(function ConnectionsChart({
  data,
}: SubChartProps): React.JSX.Element {
  return (
    <Card>
      <CardHeader>
        <CardTitle>Connections</CardTitle>
        <CardDescription>Active, idle, and waiting connections</CardDescription>
      </CardHeader>
      <CardContent>
        {data.length > 0 ? (
          <div className="h-[200px]">
            <ResponsiveContainer width="100%" height="100%">
              <AreaChart data={data} margin={CHART_MARGIN}>
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
                <Legend />
                <Area
                  type="monotone"
                  dataKey="activeConnections"
                  name="Active"
                  stackId="1"
                  stroke="#22c55e"
                  fill="#22c55e"
                  fillOpacity={0.6}
                />
                <Area
                  type="monotone"
                  dataKey="idleConnections"
                  name="Idle"
                  stackId="1"
                  stroke="#3b82f6"
                  fill="#3b82f6"
                  fillOpacity={0.6}
                />
                <Area
                  type="monotone"
                  dataKey="waitingClients"
                  name="Waiting"
                  stackId="1"
                  stroke="#f59e0b"
                  fill="#f59e0b"
                  fillOpacity={0.6}
                />
              </AreaChart>
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

const QueryThroughputChart = React.memo(function QueryThroughputChart({
  data,
}: SubChartProps): React.JSX.Element {
  return (
    <Card>
      <CardHeader>
        <CardTitle>Query Throughput</CardTitle>
        <CardDescription>Queries processed per second</CardDescription>
      </CardHeader>
      <CardContent>
        {data.length > 0 ? (
          <div className="h-[200px]">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={data} margin={CHART_MARGIN}>
                <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                <XAxis
                  dataKey="time"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                  tickFormatter={(value) => `${value}`}
                />
                <Tooltip
                  contentStyle={TOOLTIP_CONTENT_STYLE}
                  labelStyle={TOOLTIP_LABEL_STYLE}
                  formatter={(value: number) => [`${value.toFixed(1)} QPS`, "Throughput"]}
                />
                <Bar
                  dataKey="queriesPerSecond"
                  name="QPS"
                  fill="hsl(var(--primary))"
                  radius={[4, 4, 0, 0]}
                />
              </BarChart>
            </ResponsiveContainer>
          </div>
        ) : (
          <div className="flex h-[200px] items-center justify-center text-muted-foreground">
            No throughput data available
          </div>
        )}
      </CardContent>
    </Card>
  );
});

const LatencyChart = React.memo(function LatencyChart({
  data,
}: SubChartProps): React.JSX.Element {
  return (
    <Card>
      <CardHeader>
        <CardTitle>Query Latency</CardTitle>
        <CardDescription>Average query latency in milliseconds</CardDescription>
      </CardHeader>
      <CardContent>
        {data.length > 0 ? (
          <div className="h-[200px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={data} margin={CHART_MARGIN}>
                <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
                <XAxis
                  dataKey="time"
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                />
                <YAxis
                  tick={{ fontSize: 12 }}
                  className="text-muted-foreground"
                  tickFormatter={(value) => `${value}ms`}
                />
                <Tooltip
                  contentStyle={TOOLTIP_CONTENT_STYLE}
                  labelStyle={TOOLTIP_LABEL_STYLE}
                  formatter={(value: number) => [`${value.toFixed(2)}ms`, "Latency"]}
                />
                <Line
                  type="monotone"
                  dataKey="avgLatencyMs"
                  name="Avg Latency"
                  stroke="#f97316"
                  strokeWidth={2}
                  dot={false}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        ) : (
          <div className="flex h-[200px] items-center justify-center text-muted-foreground">
            No latency data available
          </div>
        )}
      </CardContent>
    </Card>
  );
});

export { ConnectionsChart, QueryThroughputChart, LatencyChart };
