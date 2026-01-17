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
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";

export type ChartType = "line" | "area" | "bar";

export interface TimeSeriesDataPoint {
  timestamp: string;
  [key: string]: string | number;
}

export interface TimeSeriesChartProps {
  title?: string;
  description?: string;
  data: TimeSeriesDataPoint[];
  type?: ChartType;
  dataKeys: Array<{
    key: string;
    name: string;
    color: string;
  }>;
  height?: number;
  isLoading?: boolean;
  formatYAxis?: (value: number) => string;
  formatXAxis?: (value: string) => string;
}

const CHART_MARGIN = { top: 5, right: 30, left: 20, bottom: 5 };
const TOOLTIP_CONTENT_STYLE = {
  backgroundColor: "hsl(var(--card))",
  border: "1px solid hsl(var(--border))",
  borderRadius: "var(--radius)",
};
const TOOLTIP_LABEL_STYLE = { color: "hsl(var(--foreground))" };

export function TimeSeriesChart({
  title,
  description,
  data,
  type = "line",
  dataKeys,
  height = 300,
  isLoading,
  formatYAxis,
  formatXAxis,
}: TimeSeriesChartProps): React.JSX.Element {
  const chartData = React.useMemo(() => {
    return data.map((point) => {
      const formattedPoint: TimeSeriesDataPoint = {
        ...point,
        time: formatXAxis
          ? formatXAxis(point.timestamp)
          : new Date(point.timestamp).toLocaleTimeString([], {
              hour: "2-digit",
              minute: "2-digit",
            }),
      };
      return formattedPoint;
    });
  }, [data, formatXAxis]);

  if (isLoading) {
    return (
      <Card>
        {(title || description) && (
          <CardHeader>
            {title && <Skeleton className="h-6 w-32" />}
            {description && <Skeleton className="h-4 w-48 mt-2" />}
          </CardHeader>
        )}
        <CardContent>
          <Skeleton className={`h-[${height}px] w-full`} />
        </CardContent>
      </Card>
    );
  }

  const renderChart = () => {
    const commonProps = {
      data: chartData,
      margin: CHART_MARGIN,
    };

    const commonAxisProps = {
      tick: { fontSize: 12 },
      className: "text-muted-foreground",
    };

    switch (type) {
      case "area":
        return (
          <AreaChart {...commonProps}>
            <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
            <XAxis dataKey="time" {...commonAxisProps} />
            <YAxis {...commonAxisProps} tickFormatter={formatYAxis} />
            <Tooltip contentStyle={TOOLTIP_CONTENT_STYLE} labelStyle={TOOLTIP_LABEL_STYLE} />
            <Legend />
            {dataKeys.map((dk) => (
              <Area
                key={dk.key}
                type="monotone"
                dataKey={dk.key}
                name={dk.name}
                stroke={dk.color}
                fill={dk.color}
                fillOpacity={0.3}
                strokeWidth={2}
              />
            ))}
          </AreaChart>
        );

      case "bar":
        return (
          <BarChart {...commonProps}>
            <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
            <XAxis dataKey="time" {...commonAxisProps} />
            <YAxis {...commonAxisProps} tickFormatter={formatYAxis} />
            <Tooltip contentStyle={TOOLTIP_CONTENT_STYLE} labelStyle={TOOLTIP_LABEL_STYLE} />
            <Legend />
            {dataKeys.map((dk) => (
              <Bar key={dk.key} dataKey={dk.key} name={dk.name} fill={dk.color} />
            ))}
          </BarChart>
        );

      case "line":
      default:
        return (
          <LineChart {...commonProps}>
            <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
            <XAxis dataKey="time" {...commonAxisProps} />
            <YAxis {...commonAxisProps} tickFormatter={formatYAxis} />
            <Tooltip contentStyle={TOOLTIP_CONTENT_STYLE} labelStyle={TOOLTIP_LABEL_STYLE} />
            <Legend />
            {dataKeys.map((dk) => (
              <Line
                key={dk.key}
                type="monotone"
                dataKey={dk.key}
                name={dk.name}
                stroke={dk.color}
                strokeWidth={2}
                dot={false}
              />
            ))}
          </LineChart>
        );
    }
  };

  const content = (
    <div style={{ height: `${height}px` }}>
      <ResponsiveContainer width="100%" height="100%">
        {renderChart()}
      </ResponsiveContainer>
    </div>
  );

  if (!title && !description) {
    return content;
  }

  return (
    <Card>
      <CardHeader>
        {title && <CardTitle>{title}</CardTitle>}
        {description && <CardDescription>{description}</CardDescription>}
      </CardHeader>
      <CardContent>{content}</CardContent>
    </Card>
  );
}
