"use client";

import * as React from "react";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";

interface DeleteClusterDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  clusterName: string;
  onDelete: () => Promise<void>;
  isDeleting: boolean;
}

export function DeleteClusterDialog({
  open,
  onOpenChange,
  clusterName,
  onDelete,
  isDeleting,
}: DeleteClusterDialogProps) {
  const [confirmation, setConfirmation] = React.useState("");

  // Reset confirmation when dialog closes
  React.useEffect(() => {
    if (!open) {
      setConfirmation("");
    }
  }, [open]);

  const handleDelete = async () => {
    if (confirmation !== clusterName) return;
    await onDelete();
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>Delete Cluster</DialogTitle>
          <DialogDescription>
            This action cannot be undone. This will permanently delete the
            cluster and all associated data.
          </DialogDescription>
        </DialogHeader>
        <div className="space-y-4 py-4">
          <p className="text-sm">
            Please type <span className="font-bold">{clusterName}</span> to
            confirm.
          </p>
          <Input
            value={confirmation}
            onChange={(e) => setConfirmation(e.target.value)}
            placeholder="Enter cluster name"
          />
        </div>
        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            Cancel
          </Button>
          <Button
            variant="destructive"
            onClick={handleDelete}
            disabled={confirmation !== clusterName || isDeleting}
          >
            Delete Cluster
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
