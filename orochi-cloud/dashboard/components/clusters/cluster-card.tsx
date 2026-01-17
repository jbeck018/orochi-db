"use client";

import * as React from "react";
import { Link } from "@tanstack/react-router";
import {
  Database,
  MoreVertical,
  Play,
  Square,
  RefreshCw,
  Trash2,
  Settings,
  ExternalLink,
  Copy
} from "lucide-react";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Progress } from "@/components/ui/progress";
import { useToast } from "@/hooks/use-toast";
import { clusterApi } from "@/lib/api";
import { formatRelativeTime, copyToClipboard, getStatusColor } from "@/lib/utils";
import type { Cluster, ClusterMetrics } from "@/types";

interface ClusterCardProps {
  cluster: Cluster;
  metrics?: ClusterMetrics;
  onRefresh?: () => void;
}

// Memoize ClusterCard to prevent re-renders when props haven't changed
export const ClusterCard = React.memo(function ClusterCard({ cluster, metrics, onRefresh }: ClusterCardProps): React.JSX.Element {
  const { toast } = useToast();
  const [isLoading, setIsLoading] = React.useState(false);

  const handleAction = async (action: "start" | "stop" | "restart" | "delete"): Promise<void> => {
    setIsLoading(true);
    try {
      switch (action) {
        case "start":
          await clusterApi.start(cluster.id);
          toast({ title: "Cluster starting", description: `${cluster.name} is starting up` });
          break;
        case "stop":
          await clusterApi.stop(cluster.id);
          toast({ title: "Cluster stopping", description: `${cluster.name} is shutting down` });
          break;
        case "restart":
          await clusterApi.restart(cluster.id);
          toast({ title: "Cluster restarting", description: `${cluster.name} is restarting` });
          break;
        case "delete":
          await clusterApi.delete(cluster.id);
          toast({ title: "Cluster deleted", description: `${cluster.name} has been deleted` });
          break;
      }
      onRefresh?.();
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "An error occurred",
        variant: "destructive",
      });
    } finally {
      setIsLoading(false);
    }
  };

  const handleCopyConnectionString = async (): Promise<void> => {
    await copyToClipboard(cluster.connectionString);
    toast({ title: "Copied", description: "Connection string copied to clipboard" });
  };

  const getStatusBadge = (): React.JSX.Element => {
    const colorClass = getStatusColor(cluster.status);
    return (
      <Badge variant="outline" className={colorClass}>
        <span className="mr-1.5 h-2 w-2 rounded-full bg-current" />
        {cluster.status.charAt(0).toUpperCase() + cluster.status.slice(1)}
      </Badge>
    );
  };

  const getTierBadge = (): React.JSX.Element => {
    const colors: Record<string, string> = {
      free: "bg-gray-100 text-gray-800 dark:bg-gray-800 dark:text-gray-200",
      standard: "bg-blue-100 text-blue-800 dark:bg-blue-900 dark:text-blue-200",
      professional: "bg-purple-100 text-purple-800 dark:bg-purple-900 dark:text-purple-200",
      enterprise: "bg-amber-100 text-amber-800 dark:bg-amber-900 dark:text-amber-200",
    };
    return (
      <Badge className={colors[cluster.config.tier]}>
        {cluster.config.tier.charAt(0).toUpperCase() + cluster.config.tier.slice(1)}
      </Badge>
    );
  };

  return (
    <Card className="hover:shadow-md transition-shadow">
      <CardHeader className="pb-2">
        <div className="flex items-start justify-between">
          <div className="flex items-center gap-3">
            <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-primary/10">
              <Database className="h-5 w-5 text-primary" />
            </div>
            <div>
              <CardTitle className="text-lg">
                <Link to="/clusters/$id" params={{ id: cluster.id }} className="hover:underline">
                  {cluster.name}
                </Link>
              </CardTitle>
              <CardDescription className="text-xs">
                {cluster.config.provider.toUpperCase()} - {cluster.config.region}
              </CardDescription>
            </div>
          </div>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" disabled={isLoading}>
                <MoreVertical className="h-4 w-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              <DropdownMenuItem asChild>
                <Link to="/clusters/$id" params={{ id: cluster.id }}>
                  <ExternalLink className="mr-2 h-4 w-4" />
                  View Details
                </Link>
              </DropdownMenuItem>
              <DropdownMenuItem asChild>
                <Link to="/clusters/$id/settings" params={{ id: cluster.id }}>
                  <Settings className="mr-2 h-4 w-4" />
                  Settings
                </Link>
              </DropdownMenuItem>
              <DropdownMenuItem onClick={handleCopyConnectionString}>
                <Copy className="mr-2 h-4 w-4" />
                Copy Connection String
              </DropdownMenuItem>
              <DropdownMenuSeparator />
              {cluster.status === "stopped" && (
                <DropdownMenuItem onClick={() => handleAction("start")}>
                  <Play className="mr-2 h-4 w-4" />
                  Start
                </DropdownMenuItem>
              )}
              {cluster.status === "running" && (
                <>
                  <DropdownMenuItem onClick={() => handleAction("stop")}>
                    <Square className="mr-2 h-4 w-4" />
                    Stop
                  </DropdownMenuItem>
                  <DropdownMenuItem onClick={() => handleAction("restart")}>
                    <RefreshCw className="mr-2 h-4 w-4" />
                    Restart
                  </DropdownMenuItem>
                </>
              )}
              <DropdownMenuSeparator />
              <DropdownMenuItem
                onClick={() => handleAction("delete")}
                className="text-destructive focus:text-destructive"
              >
                <Trash2 className="mr-2 h-4 w-4" />
                Delete
              </DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      </CardHeader>
      <CardContent className="space-y-4">
        <div className="flex items-center gap-2">
          {getStatusBadge()}
          {getTierBadge()}
        </div>

        {metrics && cluster.status === "running" && (
          <div className="space-y-3">
            <div className="space-y-1">
              <div className="flex justify-between text-sm">
                <span className="text-muted-foreground">CPU</span>
                <span>{metrics.cpu.usagePercent.toFixed(1)}%</span>
              </div>
              <Progress value={metrics.cpu.usagePercent} className="h-1.5" />
            </div>
            <div className="space-y-1">
              <div className="flex justify-between text-sm">
                <span className="text-muted-foreground">Memory</span>
                <span>{metrics.memory.usagePercent.toFixed(1)}%</span>
              </div>
              <Progress value={metrics.memory.usagePercent} className="h-1.5" />
            </div>
            <div className="space-y-1">
              <div className="flex justify-between text-sm">
                <span className="text-muted-foreground">Storage</span>
                <span>{metrics.storage.usagePercent.toFixed(1)}%</span>
              </div>
              <Progress value={metrics.storage.usagePercent} className="h-1.5" />
            </div>
          </div>
        )}

        <div className="flex items-center justify-between pt-2 border-t text-xs text-muted-foreground">
          <span>PostgreSQL {cluster.version}</span>
          <span>Updated {formatRelativeTime(cluster.updatedAt)}</span>
        </div>
      </CardContent>
    </Card>
  );
});
