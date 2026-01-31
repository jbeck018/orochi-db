"use client";

import * as React from "react";
import { Link } from "@tanstack/react-router";
import {
  Activity,
  Database,
  RefreshCw,
  Settings,
  Zap,
  AlertCircle,
  CheckCircle,
  XCircle,
  Users,
  Clock,
} from "lucide-react";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Progress } from "@/components/ui/progress";
import { Skeleton } from "@/components/ui/skeleton";
import { useToast } from "@/hooks/use-toast";
import {
  usePoolerStatus,
  useReloadPoolerConfig,
  usePausePooler,
  useResumePooler,
} from "@/hooks/api";
import { cn } from "@/lib/utils";

interface PoolerStatusCardProps {
  clusterId: string;
  compact?: boolean;
}

export const PoolerStatusCard = React.memo(function PoolerStatusCard({
  clusterId,
  compact = false,
}: PoolerStatusCardProps): React.JSX.Element {
  const { toast } = useToast();

  const { data: statusData, isLoading, error } = usePoolerStatus(clusterId);
  const status = statusData?.data;

  const reloadMutation = useReloadPoolerConfig(clusterId);
  const pauseMutation = usePausePooler(clusterId);
  const resumeMutation = useResumePooler(clusterId);

  const isActionLoading =
    reloadMutation.isPending ||
    pauseMutation.isPending ||
    resumeMutation.isPending;

  const handleReload = async (): Promise<void> => {
    try {
      await reloadMutation.mutateAsync();
      toast({
        title: "Configuration reloaded",
        description: "Pooler configuration has been reloaded successfully",
      });
    } catch (err) {
      toast({
        title: "Reload failed",
        description: err instanceof Error ? err.message : "Failed to reload configuration",
        variant: "destructive",
      });
    }
  };

  const handlePauseResume = async (): Promise<void> => {
    try {
      if (status?.healthy) {
        await pauseMutation.mutateAsync();
        toast({
          title: "Pooler paused",
          description: "Connection pooler is no longer accepting new connections",
        });
      } else {
        await resumeMutation.mutateAsync();
        toast({
          title: "Pooler resumed",
          description: "Connection pooler is now accepting connections",
        });
      }
    } catch (err) {
      toast({
        title: "Action failed",
        description: err instanceof Error ? err.message : "Operation failed",
        variant: "destructive",
      });
    }
  };

  const getHealthBadge = (healthy: boolean | undefined): React.JSX.Element => {
    if (healthy === undefined) {
      return (
        <Badge variant="outline" className="bg-gray-100 text-gray-700">
          Unknown
        </Badge>
      );
    }
    return healthy ? (
      <Badge variant="outline" className="bg-green-100 text-green-700 dark:bg-green-900 dark:text-green-300">
        <CheckCircle className="mr-1 h-3 w-3" />
        Healthy
      </Badge>
    ) : (
      <Badge variant="outline" className="bg-red-100 text-red-700 dark:bg-red-900 dark:text-red-300">
        <XCircle className="mr-1 h-3 w-3" />
        Unhealthy
      </Badge>
    );
  };

  if (isLoading) {
    return (
      <Card>
        <CardHeader className="pb-2">
          <div className="flex items-center justify-between">
            <Skeleton className="h-6 w-32" />
            <Skeleton className="h-5 w-20" />
          </div>
        </CardHeader>
        <CardContent>
          <div className="grid gap-4 md:grid-cols-4">
            {[1, 2, 3, 4].map((i) => (
              <Skeleton key={i} className="h-16" />
            ))}
          </div>
        </CardContent>
      </Card>
    );
  }

  if (error || !status) {
    return (
      <Card>
        <CardHeader>
          <CardTitle className="flex items-center gap-2">
            <Database className="h-5 w-5" />
            Connection Pooler
          </CardTitle>
        </CardHeader>
        <CardContent>
          <div className="flex items-center gap-2 text-muted-foreground">
            <AlertCircle className="h-4 w-4" />
            <span>
              {status?.config.enabled === false
                ? "Connection pooler is disabled"
                : "Unable to fetch pooler status"}
            </span>
          </div>
          <Button variant="outline" className="mt-4" asChild>
            <Link to="/clusters/$id/pooler" params={{ id: clusterId }}>
              <Settings className="mr-2 h-4 w-4" />
              Configure Pooler
            </Link>
          </Button>
        </CardContent>
      </Card>
    );
  }

  if (compact) {
    return (
      <Card className="hover:shadow-md transition-shadow">
        <CardContent className="pt-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-3">
              <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-blue-500/10">
                <Zap className="h-5 w-5 text-blue-500" />
              </div>
              <div>
                <p className="font-medium">PgDog Pooler</p>
                <div className="flex items-center gap-2 text-sm text-muted-foreground">
                  {getHealthBadge(status.healthy)}
                  <span>{status.config.mode} mode</span>
                </div>
              </div>
            </div>
            <div className="text-right">
              <p className="text-2xl font-bold">{status.stats.activeConnections}</p>
              <p className="text-xs text-muted-foreground">active connections</p>
            </div>
          </div>
          <div className="mt-4 space-y-2">
            <div className="flex justify-between text-sm">
              <span className="text-muted-foreground">Pool Utilization</span>
              <span>{status.stats.poolUtilization.toFixed(1)}%</span>
            </div>
            <Progress value={status.stats.poolUtilization} className="h-1.5" />
          </div>
          <Button variant="outline" className="mt-4 w-full" asChild>
            <Link to="/clusters/$id/pooler" params={{ id: clusterId }}>
              <Settings className="mr-2 h-4 w-4" />
              Manage Pooler
            </Link>
          </Button>
        </CardContent>
      </Card>
    );
  }

  return (
    <Card>
      <CardHeader className="pb-2">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-2">
            <Zap className="h-5 w-5 text-blue-500" />
            <CardTitle>PgDog Connection Pooler</CardTitle>
          </div>
          <div className="flex items-center gap-2">
            {getHealthBadge(status.healthy)}
            <Button
              variant="outline"
              size="sm"
              onClick={handleReload}
              disabled={isActionLoading}
            >
              <RefreshCw
                className={cn("h-4 w-4", reloadMutation.isPending && "animate-spin")}
              />
            </Button>
          </div>
        </div>
        <CardDescription>
          {status.config.mode} pooling mode
          {status.config.readWriteSplit && " with read/write splitting"}
          {status.config.shardingEnabled && ` across ${status.config.shardCount || "N/A"} shards`}
        </CardDescription>
      </CardHeader>
      <CardContent className="space-y-4">
        {/* Quick Stats Grid */}
        <div className="grid gap-4 md:grid-cols-4">
          <div className="rounded-lg border p-3">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <Users className="h-4 w-4" />
              Active Connections
            </div>
            <p className="mt-1 text-2xl font-bold">{status.stats.activeConnections}</p>
            <p className="text-xs text-muted-foreground">
              {status.stats.idleConnections} idle / {status.stats.waitingClients} waiting
            </p>
          </div>

          <div className="rounded-lg border p-3">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <Activity className="h-4 w-4" />
              Queries/sec
            </div>
            <p className="mt-1 text-2xl font-bold">
              {status.stats.queriesPerSecond.toFixed(1)}
            </p>
            <p className="text-xs text-muted-foreground">
              {status.stats.totalQueries.toLocaleString()} total
            </p>
          </div>

          <div className="rounded-lg border p-3">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <Clock className="h-4 w-4" />
              Latency
            </div>
            <p className="mt-1 text-2xl font-bold">
              {status.stats.avgLatencyMs.toFixed(1)}ms
            </p>
            <p className="text-xs text-muted-foreground">
              p99: {status.stats.p99LatencyMs.toFixed(1)}ms
            </p>
          </div>

          <div className="rounded-lg border p-3">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <Database className="h-4 w-4" />
              Pool Utilization
            </div>
            <p className="mt-1 text-2xl font-bold">
              {status.stats.poolUtilization.toFixed(1)}%
            </p>
            <Progress value={status.stats.poolUtilization} className="mt-2 h-1.5" />
          </div>
        </div>

        {/* Config Summary */}
        <div className="rounded-lg border p-3">
          <p className="text-sm font-medium mb-2">Configuration</p>
          <div className="grid grid-cols-2 md:grid-cols-4 gap-2 text-sm">
            <div>
              <span className="text-muted-foreground">Pool Size: </span>
              <span className="font-mono">
                {status.config.minPoolSize}-{status.config.maxPoolSize}
              </span>
            </div>
            <div>
              <span className="text-muted-foreground">Idle Timeout: </span>
              <span className="font-mono">{status.config.idleTimeoutSeconds}s</span>
            </div>
            <div>
              <span className="text-muted-foreground">Replicas: </span>
              <span className="font-mono">
                {status.readyReplicas}/{status.replicas}
              </span>
            </div>
            {status.version && (
              <div>
                <span className="text-muted-foreground">Version: </span>
                <span className="font-mono">{status.version}</span>
              </div>
            )}
          </div>
        </div>

        {/* Actions */}
        <div className="flex gap-2">
          <Button variant="outline" className="flex-1" asChild>
            <Link to="/clusters/$id/pooler" params={{ id: clusterId }}>
              <Settings className="mr-2 h-4 w-4" />
              Configure
            </Link>
          </Button>
          <Button
            variant="outline"
            onClick={handlePauseResume}
            disabled={isActionLoading}
          >
            {status.healthy ? "Pause" : "Resume"}
          </Button>
        </div>
      </CardContent>
    </Card>
  );
});
