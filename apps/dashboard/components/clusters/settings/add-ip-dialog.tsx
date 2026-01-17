"use client";

import * as React from "react";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import type { IpAllowEntry } from "@/types";

interface AddIpDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onAdd: (entry: IpAllowEntry) => void;
}

export function AddIpDialog({ open, onOpenChange, onAdd }: AddIpDialogProps) {
  const [cidr, setCidr] = React.useState("");
  const [description, setDescription] = React.useState("");

  // Reset form when dialog closes
  React.useEffect(() => {
    if (!open) {
      setCidr("");
      setDescription("");
    }
  }, [open]);

  const handleAdd = () => {
    if (!cidr.trim()) return;

    const newEntry: IpAllowEntry = {
      id: crypto.randomUUID(),
      cidr: cidr.trim(),
      description: description.trim(),
      createdAt: new Date().toISOString(),
    };

    onAdd(newEntry);
    onOpenChange(false);
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>Add IP Address</DialogTitle>
          <DialogDescription>
            Add an IP address or CIDR range to the allow list
          </DialogDescription>
        </DialogHeader>
        <div className="space-y-4 py-4">
          <div className="space-y-2">
            <Label htmlFor="ip-cidr">IP Address or CIDR</Label>
            <Input
              id="ip-cidr"
              value={cidr}
              onChange={(e) => setCidr(e.target.value)}
              placeholder="192.168.1.0/24 or 10.0.0.1"
            />
            <p className="text-xs text-muted-foreground">
              Use CIDR notation for ranges (e.g., 192.168.1.0/24) or single IPs
            </p>
          </div>
          <div className="space-y-2">
            <Label htmlFor="ip-description">Description (optional)</Label>
            <Input
              id="ip-description"
              value={description}
              onChange={(e) => setDescription(e.target.value)}
              placeholder="Office network"
            />
          </div>
        </div>
        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            Cancel
          </Button>
          <Button onClick={handleAdd} disabled={!cidr.trim()}>
            Add IP
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
