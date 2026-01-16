"use client";

import * as React from "react";
import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import { ArrowLeft, Loader2, Save, AlertCircle, Trash2 } from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
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
import { Switch } from "@/components/ui/switch";
import { Skeleton } from "@/components/ui/skeleton";
import { Separator } from "@/components/ui/separator";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { useToast } from "@/hooks/use-toast";
import { clusterApi } from "@/lib/api";
import type { Cluster, UpdateClusterForm } from "@/types";

export default function ClusterSettingsPage(): React.JSX.Element {
  const params = useParams();
  const router = useRouter();
  const { toast } = useToast();
  const clusterId = params.id as string;

  const [cluster, setCluster] = React.useState<Cluster | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);
  const [isSaving, setIsSaving] = React.useState(false);
  const [showDeleteDialog, setShowDeleteDialog] = React.useState(false);
  const [deleteConfirmation, setDeleteConfirmation] = React.useState("");

  const [formData, setFormData] = React.useState<UpdateClusterForm>({});
  const [hasChanges, setHasChanges] = React.useState(false);

  const fetchCluster = React.useCallback(async (): Promise<void> => {
    try {
      const response = await clusterApi.get(clusterId);
      setCluster(response.data);
      setFormData({
        name: response.data.name,
        nodeCount: response.data.config.nodeCount,
        storageGb: response.data.config.storageGb,
        backupEnabled: response.data.config.backupEnabled,
        backupRetentionDays: response.data.config.backupRetentionDays,
        maintenanceWindow: response.data.config.maintenanceWindow,
      });
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to load cluster settings",
        variant: "destructive",
      });
    } finally {
      setIsLoading(false);
    }
  }, [clusterId, toast]);

  React.useEffect(() => {
    fetchCluster();
  }, [fetchCluster]);

  const handleChange = <K extends keyof UpdateClusterForm>(
    key: K,
    value: UpdateClusterForm[K]
  ): void => {
    setFormData((prev) => ({ ...prev, [key]: value }));
    setHasChanges(true);
  };

  const handleSave = async (): Promise<void> => {
    setIsSaving(true);
    try {
      await clusterApi.update(clusterId, formData);
      toast({ title: "Settings saved" });
      setHasChanges(false);
      fetchCluster();
    } catch (error) {
      toast({
        title: "Error",
        description: error instanceof Error ? error.message : "Failed to save",
        variant: "destructive",
      });
    } finally {
      setIsSaving(false);
    }
  };

  const handleDelete = async (): Promise<void> => {
    if (deleteConfirmation !== cluster?.name) return;

    setIsSaving(true);
    try {
      await clusterApi.delete(clusterId);
      toast({ title: "Cluster deleted" });
      router.push("/clusters");
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to delete cluster",
        variant: "destructive",
      });
    } finally {
      setIsSaving(false);
      setShowDeleteDialog(false);
    }
  };

  if (isLoading) {
    return (
      <DashboardLayout>
        <div className="max-w-4xl mx-auto space-y-6">
          <Skeleton className="h-10 w-64" />
          <Skeleton className="h-[200px]" />
          <Skeleton className="h-[200px]" />
        </div>
      </DashboardLayout>
    );
  }

  if (!cluster) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <AlertCircle className="h-12 w-12 text-muted-foreground mb-4" />
          <h2 className="text-xl font-semibold">Cluster not found</h2>
          <Button asChild className="mt-4">
            <Link href="/clusters">Back to Clusters</Link>
          </Button>
        </div>
      </DashboardLayout>
    );
  }

  return (
    <DashboardLayout>
      <div className="max-w-4xl mx-auto space-y-6">
        {/* Header */}
        <div className="flex items-center gap-4">
          <Button variant="ghost" size="icon" asChild>
            <Link href={`/clusters/${clusterId}`}>
              <ArrowLeft className="h-5 w-5" />
            </Link>
          </Button>
          <div>
            <h1 className="text-3xl font-bold tracking-tight">
              Cluster Settings
            </h1>
            <p className="text-muted-foreground">{cluster.name}</p>
          </div>
        </div>

        {hasChanges && (
          <Alert>
            <AlertCircle className="h-4 w-4" />
            <AlertDescription>
              You have unsaved changes. Click "Save Changes" to apply them.
            </AlertDescription>
          </Alert>
        )}

        {/* General Settings */}
        <Card>
          <CardHeader>
            <CardTitle>General Settings</CardTitle>
            <CardDescription>
              Basic cluster configuration options
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-4">
            <div className="space-y-2">
              <Label htmlFor="name">Cluster Name</Label>
              <Input
                id="name"
                value={formData.name ?? ""}
                onChange={(e) => handleChange("name", e.target.value)}
                disabled={isSaving}
              />
            </div>
          </CardContent>
        </Card>

        {/* Compute & Storage */}
        <Card>
          <CardHeader>
            <CardTitle>Compute & Storage</CardTitle>
            <CardDescription>
              Adjust compute and storage resources
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-4">
            <div className="grid grid-cols-2 gap-4">
              <div className="space-y-2">
                <Label htmlFor="nodeCount">Node Count</Label>
                <Select
                  value={formData.nodeCount?.toString()}
                  onValueChange={(value) =>
                    handleChange("nodeCount", parseInt(value, 10))
                  }
                  disabled={isSaving || cluster.config.tier === "free"}
                >
                  <SelectTrigger id="nodeCount">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {[1, 2, 3, 4, 5, 6, 7, 8].map((count) => (
                      <SelectItem key={count} value={count.toString()}>
                        {count} {count === 1 ? "node" : "nodes"}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
                {cluster.config.tier === "free" && (
                  <p className="text-xs text-muted-foreground">
                    Upgrade to scale beyond 1 node
                  </p>
                )}
              </div>
              <div className="space-y-2">
                <Label htmlFor="storage">Storage (GB)</Label>
                <Select
                  value={formData.storageGb?.toString()}
                  onValueChange={(value) =>
                    handleChange("storageGb", parseInt(value, 10))
                  }
                  disabled={isSaving || cluster.config.tier === "free"}
                >
                  <SelectTrigger id="storage">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {[10, 25, 50, 100, 200, 500, 1000, 2000].map((size) => (
                      <SelectItem key={size} value={size.toString()}>
                        {size >= 1000 ? `${size / 1000} TB` : `${size} GB`}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            </div>

            <Alert>
              <AlertCircle className="h-4 w-4" />
              <AlertDescription>
                Changing compute or storage may cause a brief service interruption.
              </AlertDescription>
            </Alert>
          </CardContent>
        </Card>

        {/* Backup Settings */}
        <Card>
          <CardHeader>
            <CardTitle>Backup Settings</CardTitle>
            <CardDescription>
              Configure automated backups for your cluster
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-6">
            <div className="flex items-center justify-between">
              <div className="space-y-0.5">
                <Label>Automatic Backups</Label>
                <p className="text-sm text-muted-foreground">
                  Enable daily automated backups
                </p>
              </div>
              <Switch
                checked={formData.backupEnabled ?? false}
                onCheckedChange={(checked) =>
                  handleChange("backupEnabled", checked)
                }
                disabled={isSaving || cluster.config.tier === "free"}
              />
            </div>

            {formData.backupEnabled && (
              <div className="space-y-2">
                <Label htmlFor="retention">Backup Retention (days)</Label>
                <Select
                  value={formData.backupRetentionDays?.toString() ?? "7"}
                  onValueChange={(value) =>
                    handleChange("backupRetentionDays", parseInt(value, 10))
                  }
                  disabled={isSaving}
                >
                  <SelectTrigger id="retention">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {[1, 3, 7, 14, 30, 60, 90].map((days) => (
                      <SelectItem key={days} value={days.toString()}>
                        {days} {days === 1 ? "day" : "days"}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            )}
          </CardContent>
        </Card>

        {/* Maintenance Window */}
        <Card>
          <CardHeader>
            <CardTitle>Maintenance Window</CardTitle>
            <CardDescription>
              Schedule automatic maintenance and updates
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-4">
            <div className="grid grid-cols-2 gap-4">
              <div className="space-y-2">
                <Label>Day of Week</Label>
                <Select
                  value={formData.maintenanceWindow?.dayOfWeek?.toString() ?? "0"}
                  onValueChange={(value) =>
                    handleChange("maintenanceWindow", {
                      ...formData.maintenanceWindow,
                      dayOfWeek: parseInt(value, 10),
                      hourUtc: formData.maintenanceWindow?.hourUtc ?? 3,
                    })
                  }
                  disabled={isSaving}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {[
                      "Sunday",
                      "Monday",
                      "Tuesday",
                      "Wednesday",
                      "Thursday",
                      "Friday",
                      "Saturday",
                    ].map((day, index) => (
                      <SelectItem key={day} value={index.toString()}>
                        {day}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
              <div className="space-y-2">
                <Label>Hour (UTC)</Label>
                <Select
                  value={formData.maintenanceWindow?.hourUtc?.toString() ?? "3"}
                  onValueChange={(value) =>
                    handleChange("maintenanceWindow", {
                      ...formData.maintenanceWindow,
                      dayOfWeek: formData.maintenanceWindow?.dayOfWeek ?? 0,
                      hourUtc: parseInt(value, 10),
                    })
                  }
                  disabled={isSaving}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {Array.from({ length: 24 }, (_, i) => (
                      <SelectItem key={i} value={i.toString()}>
                        {i.toString().padStart(2, "0")}:00 UTC
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            </div>
          </CardContent>
        </Card>

        {/* Danger Zone */}
        <Card className="border-destructive">
          <CardHeader>
            <CardTitle className="text-destructive">Danger Zone</CardTitle>
            <CardDescription>
              Irreversible and destructive actions
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="flex items-center justify-between">
              <div>
                <p className="font-medium">Delete this cluster</p>
                <p className="text-sm text-muted-foreground">
                  Once deleted, this cluster cannot be recovered.
                </p>
              </div>
              <Button
                variant="destructive"
                onClick={() => setShowDeleteDialog(true)}
              >
                <Trash2 className="mr-2 h-4 w-4" />
                Delete Cluster
              </Button>
            </div>
          </CardContent>
        </Card>

        {/* Save Button */}
        <div className="flex justify-end gap-4">
          <Button variant="outline" asChild>
            <Link href={`/clusters/${clusterId}`}>Cancel</Link>
          </Button>
          <Button onClick={handleSave} disabled={!hasChanges || isSaving}>
            {isSaving ? (
              <>
                <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                Saving...
              </>
            ) : (
              <>
                <Save className="mr-2 h-4 w-4" />
                Save Changes
              </>
            )}
          </Button>
        </div>

        {/* Delete Dialog */}
        <Dialog open={showDeleteDialog} onOpenChange={setShowDeleteDialog}>
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
                Please type <span className="font-bold">{cluster.name}</span> to
                confirm.
              </p>
              <Input
                value={deleteConfirmation}
                onChange={(e) => setDeleteConfirmation(e.target.value)}
                placeholder="Enter cluster name"
              />
            </div>
            <DialogFooter>
              <Button
                variant="outline"
                onClick={() => setShowDeleteDialog(false)}
              >
                Cancel
              </Button>
              <Button
                variant="destructive"
                onClick={handleDelete}
                disabled={deleteConfirmation !== cluster.name || isSaving}
              >
                Delete Cluster
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>
      </div>
    </DashboardLayout>
  );
}
