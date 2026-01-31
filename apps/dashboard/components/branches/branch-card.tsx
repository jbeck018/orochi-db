"use client";

import * as React from "react";
import {
  GitBranch,
  MoreVertical,
  Trash2,
  ArrowUpCircle,
  Copy,
  Loader2,
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
import { useToast } from "@/hooks/use-toast";
import { useDeleteBranch, usePromoteBranch } from "@/hooks/api";
import { formatRelativeTime, copyToClipboard } from "@/lib/utils";
import type { Branch, BranchStatus } from "@/types";

interface BranchCardProps {
  branch: Branch;
  clusterId: string;
  onRefresh?: () => void;
}

const getStatusColor = (status: BranchStatus): string => {
  switch (status) {
    case "ready":
      return "text-green-600 dark:text-green-400";
    case "creating":
      return "text-blue-600 dark:text-blue-400";
    case "promoting":
      return "text-purple-600 dark:text-purple-400";
    case "deleting":
      return "text-orange-600 dark:text-orange-400";
    case "failed":
      return "text-red-600 dark:text-red-400";
    default:
      return "text-gray-600 dark:text-gray-400";
  }
};

const getMethodLabel = (method: string): string => {
  switch (method) {
    case "volumeSnapshot":
      return "Volume Snapshot";
    case "clone":
      return "PostgreSQL CLONE";
    case "pg_basebackup":
      return "pg_basebackup";
    case "pitr":
      return "Point-in-Time Recovery";
    default:
      return method;
  }
};

export const BranchCard = React.memo(function BranchCard({
  branch,
  clusterId,
  onRefresh,
}: BranchCardProps): React.JSX.Element {
  const { toast } = useToast();

  const deleteMutation = useDeleteBranch(clusterId);
  const promoteMutation = usePromoteBranch(clusterId);

  const isLoading = deleteMutation.isPending || promoteMutation.isPending;
  const isCreating = branch.status === "creating";
  const isReady = branch.status === "ready";

  const handleDelete = async (): Promise<void> => {
    try {
      await deleteMutation.mutateAsync(branch.id);
      toast({
        title: "Branch deleted",
        description: `${branch.name} has been deleted`,
      });
      onRefresh?.();
    } catch (error) {
      toast({
        title: "Error",
        description:
          error instanceof Error ? error.message : "Failed to delete branch",
        variant: "destructive",
      });
    }
  };

  const handlePromote = async (): Promise<void> => {
    try {
      await promoteMutation.mutateAsync(branch.id);
      toast({
        title: "Branch promoted",
        description: `${branch.name} is being promoted to a standalone cluster`,
      });
      onRefresh?.();
    } catch (error) {
      toast({
        title: "Error",
        description:
          error instanceof Error ? error.message : "Failed to promote branch",
        variant: "destructive",
      });
    }
  };

  const handleCopyConnectionString = async (): Promise<void> => {
    if (branch.connectionString) {
      await copyToClipboard(branch.connectionString);
      toast({
        title: "Copied",
        description: "Connection string copied to clipboard",
      });
    }
  };

  return (
    <Card className="hover:shadow-md transition-shadow">
      <CardHeader className="pb-2">
        <div className="flex items-start justify-between">
          <div className="flex items-center gap-3">
            <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-primary/10">
              {isCreating ? (
                <Loader2 className="h-5 w-5 text-primary animate-spin" />
              ) : (
                <GitBranch className="h-5 w-5 text-primary" />
              )}
            </div>
            <div>
              <CardTitle className="text-lg">{branch.name}</CardTitle>
              <CardDescription className="text-xs">
                From {branch.parentCluster}
              </CardDescription>
            </div>
          </div>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button
                variant="ghost"
                size="icon"
                disabled={isLoading || isCreating}
              >
                <MoreVertical className="h-4 w-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              {isReady && branch.connectionString && (
                <DropdownMenuItem onClick={handleCopyConnectionString}>
                  <Copy className="mr-2 h-4 w-4" />
                  Copy Connection String
                </DropdownMenuItem>
              )}
              {isReady && (
                <DropdownMenuItem onClick={handlePromote}>
                  <ArrowUpCircle className="mr-2 h-4 w-4" />
                  Promote to Cluster
                </DropdownMenuItem>
              )}
              <DropdownMenuSeparator />
              <DropdownMenuItem
                onClick={handleDelete}
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
          <Badge variant="outline" className={getStatusColor(branch.status)}>
            <span className="mr-1.5 h-2 w-2 rounded-full bg-current" />
            {branch.status.charAt(0).toUpperCase() + branch.status.slice(1)}
          </Badge>
          <Badge variant="secondary">{getMethodLabel(branch.method)}</Badge>
        </div>

        {isReady && branch.connectionString && (
          <div className="p-2 bg-muted rounded-md">
            <p className="text-xs text-muted-foreground font-mono truncate">
              {branch.connectionString}
            </p>
          </div>
        )}

        {isCreating && (
          <div className="flex items-center gap-2 text-sm text-muted-foreground">
            <Loader2 className="h-4 w-4 animate-spin" />
            Creating branch...
          </div>
        )}

        <div className="flex items-center justify-between pt-2 border-t text-xs text-muted-foreground">
          <span>ID: {branch.id.slice(0, 8)}...</span>
          <span>Created {formatRelativeTime(branch.createdAt)}</span>
        </div>
      </CardContent>
    </Card>
  );
});
