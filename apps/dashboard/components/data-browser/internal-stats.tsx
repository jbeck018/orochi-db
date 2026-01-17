"use client";

import * as React from "react";
import {
  Database,
  Layers,
  Grid3X3,
  Archive,
  HardDrive,
  RefreshCw,
  Info,
} from "lucide-react";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";
import type { InternalTableStats } from "@/types";

interface InternalStatsProps {
  stats: InternalTableStats | undefined;
  isLoading: boolean;
  onRefresh: () => void;
}

export function InternalStats({ stats, isLoading, onRefresh }: InternalStatsProps) {
  return (
    <Card>
      <CardHeader className="pb-3">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-2">
            <CardTitle className="text-base">Internal Statistics</CardTitle>
            <TooltipProvider>
              <Tooltip>
                <TooltipTrigger asChild>
                  <Info className="h-4 w-4 text-muted-foreground cursor-help" />
                </TooltipTrigger>
                <TooltipContent side="right" className="max-w-xs">
                  <p>
                    Statistics for Orochi DB internal tables including hypertables,
                    time-series chunks, and distributed shards. These tables are
                    managed automatically by the system.
                  </p>
                </TooltipContent>
              </Tooltip>
            </TooltipProvider>
          </div>
          <Button
            variant="ghost"
            size="icon"
            className="h-7 w-7"
            onClick={onRefresh}
            disabled={isLoading}
          >
            <RefreshCw
              className={cn("h-4 w-4", isLoading && "animate-spin")}
            />
          </Button>
        </div>
        <CardDescription>
          Orochi DB managed tables and storage
        </CardDescription>
      </CardHeader>
      <CardContent>
        {isLoading ? (
          <div className="grid grid-cols-2 md:grid-cols-3 gap-4">
            {Array.from({ length: 6 }).map((_, i) => (
              <Skeleton key={i} className="h-20" />
            ))}
          </div>
        ) : !stats ? (
          <div className="flex flex-col items-center justify-center py-8 text-center">
            <Database className="h-8 w-8 text-muted-foreground mb-2" />
            <p className="text-sm text-muted-foreground">
              Unable to load internal statistics
            </p>
          </div>
        ) : (
          <div className="grid grid-cols-2 md:grid-cols-3 gap-4">
            <StatCard
              icon={<Layers className="h-5 w-5" />}
              label="Hypertables"
              value={stats.hypertables}
              description="Time-series optimized tables"
            />
            <StatCard
              icon={<Grid3X3 className="h-5 w-5" />}
              label="Chunks"
              value={stats.chunks}
              description="Time-partitioned segments"
            />
            <StatCard
              icon={<Archive className="h-5 w-5" />}
              label="Compressed"
              value={stats.compressedChunks}
              description="Compressed chunks"
              variant={stats.compressedChunks > 0 ? "success" : "default"}
            />
            <StatCard
              icon={<Database className="h-5 w-5" />}
              label="Shards"
              value={stats.shardCount}
              description="Distributed data partitions"
            />
            <StatCard
              icon={<HardDrive className="h-5 w-5" />}
              label="Total Size"
              value={stats.totalSizeHuman}
              isText
              description="Combined internal storage"
            />
            <StatCard
              icon={<Archive className="h-5 w-5" />}
              label="Compression Ratio"
              value={
                stats.chunks > 0
                  ? `${Math.round((stats.compressedChunks / stats.chunks) * 100)}%`
                  : "N/A"
              }
              isText
              description="Chunks compressed"
              variant={
                stats.chunks > 0 && stats.compressedChunks / stats.chunks > 0.5
                  ? "success"
                  : "default"
              }
            />
          </div>
        )}
      </CardContent>
    </Card>
  );
}

interface StatCardProps {
  icon: React.ReactNode;
  label: string;
  value: number | string;
  description: string;
  isText?: boolean;
  variant?: "default" | "success";
}

const StatCard = React.memo(function StatCard({
  icon,
  label,
  value,
  description,
  isText = false,
  variant = "default",
}: StatCardProps) {
  return (
    <TooltipProvider>
      <Tooltip>
        <TooltipTrigger asChild>
          <div
            className={cn(
              "rounded-lg border p-3 transition-colors hover:bg-muted/50",
              variant === "success" && "border-green-500/30 bg-green-500/5"
            )}
          >
            <div className="flex items-center gap-2 mb-2">
              <span
                className={cn(
                  "text-muted-foreground",
                  variant === "success" && "text-green-600"
                )}
              >
                {icon}
              </span>
              <span className="text-sm font-medium">{label}</span>
            </div>
            <p
              className={cn(
                "text-2xl font-bold tabular-nums",
                variant === "success" && "text-green-600"
              )}
            >
              {isText ? value : typeof value === "number" ? value.toLocaleString() : value}
            </p>
          </div>
        </TooltipTrigger>
        <TooltipContent>
          <p>{description}</p>
        </TooltipContent>
      </Tooltip>
    </TooltipProvider>
  );
});
