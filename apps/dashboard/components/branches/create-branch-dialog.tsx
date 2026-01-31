"use client";

import * as React from "react";
import { Loader2 } from "lucide-react";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { useToast } from "@/hooks/use-toast";
import { useCreateBranch } from "@/hooks/api";
import type { BranchMethod, CreateBranchForm } from "@/types";

interface CreateBranchDialogProps {
  clusterId: string;
  clusterName: string;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onSuccess?: () => void;
}

const branchMethods: { value: BranchMethod; label: string; description: string }[] = [
  {
    value: "volumeSnapshot",
    label: "Volume Snapshot (Recommended)",
    description: "Instant copy using CSI volume snapshots - fastest method",
  },
  {
    value: "clone",
    label: "PostgreSQL CLONE",
    description: "Uses PostgreSQL 18's native CLONE feature",
  },
  {
    value: "pg_basebackup",
    label: "pg_basebackup",
    description: "Traditional physical backup - most compatible",
  },
  {
    value: "pitr",
    label: "Point-in-Time Recovery",
    description: "Create branch from a specific point in time",
  },
];

export function CreateBranchDialog({
  clusterId,
  clusterName,
  open,
  onOpenChange,
  onSuccess,
}: CreateBranchDialogProps): React.JSX.Element {
  const { toast } = useToast();
  const createMutation = useCreateBranch(clusterId);

  const [name, setName] = React.useState("");
  const [method, setMethod] = React.useState<BranchMethod>("volumeSnapshot");
  const [pointInTime, setPointInTime] = React.useState("");

  const handleSubmit = async (e: React.FormEvent): Promise<void> => {
    e.preventDefault();

    if (!name.trim()) {
      toast({
        title: "Validation Error",
        description: "Branch name is required",
        variant: "destructive",
      });
      return;
    }

    const data: CreateBranchForm = {
      name: name.trim(),
      method,
      inherit: true,
    };

    if (method === "pitr" && pointInTime) {
      data.pointInTime = pointInTime;
    }

    try {
      await createMutation.mutateAsync(data);
      toast({
        title: "Branch created",
        description: `Creating branch "${name}" from ${clusterName}`,
      });
      setName("");
      setMethod("volumeSnapshot");
      setPointInTime("");
      onSuccess?.();
    } catch (error) {
      toast({
        title: "Failed to create branch",
        description:
          error instanceof Error ? error.message : "An error occurred",
        variant: "destructive",
      });
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="sm:max-w-[500px]">
        <form onSubmit={handleSubmit}>
          <DialogHeader>
            <DialogTitle>Create Database Branch</DialogTitle>
            <DialogDescription>
              Create an instant copy of <strong>{clusterName}</strong> for
              development or testing. Changes to the branch won't affect the
              parent cluster.
            </DialogDescription>
          </DialogHeader>

          <div className="grid gap-4 py-4">
            <div className="grid gap-2">
              <Label htmlFor="name">Branch Name</Label>
              <Input
                id="name"
                placeholder="e.g., feature-auth, staging, dev-john"
                value={name}
                onChange={(e) => setName(e.target.value)}
                disabled={createMutation.isPending}
              />
              <p className="text-xs text-muted-foreground">
                Choose a descriptive name for your branch
              </p>
            </div>

            <div className="grid gap-2">
              <Label htmlFor="method">Cloning Method</Label>
              <Select
                value={method}
                onValueChange={(v) => setMethod(v as BranchMethod)}
                disabled={createMutation.isPending}
              >
                <SelectTrigger>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {branchMethods.map((m) => (
                    <SelectItem key={m.value} value={m.value}>
                      <div className="flex flex-col">
                        <span>{m.label}</span>
                      </div>
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <p className="text-xs text-muted-foreground">
                {branchMethods.find((m) => m.value === method)?.description}
              </p>
            </div>

            {method === "pitr" && (
              <div className="grid gap-2">
                <Label htmlFor="pointInTime">Point in Time (Optional)</Label>
                <Input
                  id="pointInTime"
                  type="datetime-local"
                  value={pointInTime}
                  onChange={(e) => setPointInTime(e.target.value)}
                  disabled={createMutation.isPending}
                />
                <p className="text-xs text-muted-foreground">
                  Leave empty to use the current time
                </p>
              </div>
            )}
          </div>

          <DialogFooter>
            <Button
              type="button"
              variant="outline"
              onClick={() => onOpenChange(false)}
              disabled={createMutation.isPending}
            >
              Cancel
            </Button>
            <Button type="submit" disabled={createMutation.isPending}>
              {createMutation.isPending && (
                <Loader2 className="mr-2 h-4 w-4 animate-spin" />
              )}
              Create Branch
            </Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  );
}
