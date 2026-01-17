import * as React from "react";
import { Badge } from "@/components/ui/badge";
import { cn } from "@/lib/utils";

export type HealthStatus = "healthy" | "degraded" | "unhealthy" | "unknown" | "active" | "paused" | "error" | "running" | "stopped";

export interface StatusBadgeProps {
  status: HealthStatus;
  className?: string;
}

const statusConfig: Record<HealthStatus, { label: string; variant: "default" | "secondary" | "destructive" | "outline"; className: string }> = {
  healthy: {
    label: "Healthy",
    variant: "default",
    className: "bg-green-500 hover:bg-green-600 text-white",
  },
  active: {
    label: "Active",
    variant: "default",
    className: "bg-green-500 hover:bg-green-600 text-white",
  },
  running: {
    label: "Running",
    variant: "default",
    className: "bg-green-500 hover:bg-green-600 text-white",
  },
  degraded: {
    label: "Degraded",
    variant: "secondary",
    className: "bg-yellow-500 hover:bg-yellow-600 text-white",
  },
  paused: {
    label: "Paused",
    variant: "secondary",
    className: "bg-yellow-500 hover:bg-yellow-600 text-white",
  },
  unhealthy: {
    label: "Unhealthy",
    variant: "destructive",
    className: "bg-red-500 hover:bg-red-600 text-white",
  },
  error: {
    label: "Error",
    variant: "destructive",
    className: "bg-red-500 hover:bg-red-600 text-white",
  },
  stopped: {
    label: "Stopped",
    variant: "outline",
    className: "bg-gray-500 hover:bg-gray-600 text-white",
  },
  unknown: {
    label: "Unknown",
    variant: "outline",
    className: "bg-gray-500 hover:bg-gray-600 text-white",
  },
};

export function StatusBadge({ status, className }: StatusBadgeProps): React.JSX.Element {
  const config = statusConfig[status] || statusConfig.unknown;

  return (
    <Badge
      variant={config.variant}
      className={cn(config.className, className)}
    >
      {config.label}
    </Badge>
  );
}
