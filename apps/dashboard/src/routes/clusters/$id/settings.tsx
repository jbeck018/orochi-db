import * as React from "react";
import { createFileRoute, Link, useNavigate } from "@tanstack/react-router";
import {
  ArrowLeft,
  Loader2,
  Save,
  AlertCircle,
  Trash2,
  Plus,
  X,
  Network,
  Database,
  Shield,
  Zap,
  Server,
  Globe,
} from "lucide-react";
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
import { Slider } from "@/components/ui/slider";
import { Separator } from "@/components/ui/separator";
import { Badge } from "@/components/ui/badge";
import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { useToast } from "@/hooks/use-toast";
import { clusterApi } from "@/lib/api";
import type {
  Cluster,
  UpdateClusterForm,
  IpAllowEntry,
  PoolerMode,
} from "@/types";
import {
  DeleteClusterDialog,
  AddIpDialog,
} from "@/components/clusters/settings";

export const Route = createFileRoute("/clusters/$id/settings")({
  component: ClusterSettingsPage,
});

// Compute unit options (like NeonDB)
const COMPUTE_UNITS = [
  { value: 0.25, label: "0.25 CU", ram: "1 GB" },
  { value: 0.5, label: "0.5 CU", ram: "2 GB" },
  { value: 1, label: "1 CU", ram: "4 GB" },
  { value: 2, label: "2 CU", ram: "8 GB" },
  { value: 4, label: "4 CU", ram: "16 GB" },
  { value: 8, label: "8 CU", ram: "32 GB" },
];

function ClusterSettingsPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();
  const navigate = useNavigate();
  const { toast } = useToast();

  const [cluster, setCluster] = React.useState<Cluster | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);
  const [isSaving, setIsSaving] = React.useState(false);
  const [showDeleteDialog, setShowDeleteDialog] = React.useState(false);
  const [showAddIpDialog, setShowAddIpDialog] = React.useState(false);

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
        pooler: response.data.config.pooler ?? {
          enabled: false,
          mode: "transaction",
          maxClientConnections: 200,
          defaultPoolSize: 15,
          maxPoolSize: 50,
          idleTimeout: 300,
        },
        autoscale: response.data.config.autoscale ?? {
          enabled: false,
          minComputeUnits: 0.25,
          maxComputeUnits: 4,
          scaleToZero: false,
          scaleToZeroDelayMinutes: 5,
        },
        network: response.data.config.network ?? {
          ipAllowList: [],
          sslRequired: true,
          publicAccess: true,
        },
        readReplicas: response.data.config.readReplicas ?? {
          enabled: false,
          replicaCount: 0,
          regions: [],
        },
      });
    } catch {
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

  const handlePoolerChange = <K extends keyof NonNullable<UpdateClusterForm["pooler"]>>(
    key: K,
    value: NonNullable<UpdateClusterForm["pooler"]>[K]
  ): void => {
    setFormData((prev) => ({
      ...prev,
      pooler: { ...prev.pooler, [key]: value },
    }));
    setHasChanges(true);
  };

  const handleAutoscaleChange = <K extends keyof NonNullable<UpdateClusterForm["autoscale"]>>(
    key: K,
    value: NonNullable<UpdateClusterForm["autoscale"]>[K]
  ): void => {
    setFormData((prev) => ({
      ...prev,
      autoscale: { ...prev.autoscale, [key]: value },
    }));
    setHasChanges(true);
  };

  const handleNetworkChange = <K extends keyof NonNullable<UpdateClusterForm["network"]>>(
    key: K,
    value: NonNullable<UpdateClusterForm["network"]>[K]
  ): void => {
    setFormData((prev) => ({
      ...prev,
      network: { ...prev.network, [key]: value },
    }));
    setHasChanges(true);
  };

  const handleReadReplicasChange = <K extends keyof NonNullable<UpdateClusterForm["readReplicas"]>>(
    key: K,
    value: NonNullable<UpdateClusterForm["readReplicas"]>[K]
  ): void => {
    setFormData((prev) => ({
      ...prev,
      readReplicas: { ...prev.readReplicas, [key]: value },
    }));
    setHasChanges(true);
  };

  const handleAddIp = (newEntry: IpAllowEntry): void => {
    const currentList = formData.network?.ipAllowList ?? [];
    handleNetworkChange("ipAllowList", [...currentList, newEntry]);
  };

  const handleRemoveIp = (id: string): void => {
    const currentList = formData.network?.ipAllowList ?? [];
    handleNetworkChange(
      "ipAllowList",
      currentList.filter((entry) => entry.id !== id)
    );
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
    setIsSaving(true);
    try {
      await clusterApi.delete(clusterId);
      toast({ title: "Cluster deleted" });
      navigate({ to: "/clusters" });
    } catch {
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
      <div className="max-w-4xl mx-auto space-y-6">
        <Skeleton className="h-10 w-64" />
        <Skeleton className="h-[200px]" />
        <Skeleton className="h-[200px]" />
        <Skeleton className="h-[200px]" />
      </div>
    );
  }

  if (!cluster) {
    return (
      <div className="flex flex-col items-center justify-center py-20">
        <AlertCircle className="h-12 w-12 text-muted-foreground mb-4" />
        <h2 className="text-xl font-semibold">Cluster not found</h2>
        <Button asChild className="mt-4">
          <Link to="/clusters">Back to Clusters</Link>
        </Button>
      </div>
    );
  }

  const isFree = cluster.config.tier === "free";

  return (
    <div className="max-w-4xl mx-auto space-y-6 pb-20">
      {/* Header */}
      <div className="flex items-center gap-4">
        <Button variant="ghost" size="icon" asChild>
          <Link to="/clusters/$id" params={{ id: clusterId }}>
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
          <div className="flex items-center gap-2">
            <Database className="h-5 w-5 text-primary" />
            <CardTitle>General Settings</CardTitle>
          </div>
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

      {/* Compute & Autoscaling */}
      <Card>
        <CardHeader>
          <div className="flex items-center gap-2">
            <Zap className="h-5 w-5 text-primary" />
            <CardTitle>Compute & Autoscaling</CardTitle>
          </div>
          <CardDescription>
            Configure compute resources and autoscaling behavior
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          {/* Static compute settings */}
          <div className="grid grid-cols-2 gap-4">
            <div className="space-y-2">
              <Label htmlFor="nodeCount">Node Count</Label>
              <Select
                value={formData.nodeCount?.toString()}
                onValueChange={(value) =>
                  handleChange("nodeCount", parseInt(value, 10))
                }
                disabled={isSaving || isFree}
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
              {isFree && (
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
                disabled={isSaving || isFree}
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

          <Separator />

          {/* Autoscaling */}
          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <div className="space-y-0.5">
                <Label>Compute Autoscaling</Label>
                <p className="text-sm text-muted-foreground">
                  Automatically scale compute based on demand
                </p>
              </div>
              <Switch
                checked={formData.autoscale?.enabled ?? false}
                onCheckedChange={(checked) =>
                  handleAutoscaleChange("enabled", checked)
                }
                disabled={isSaving || isFree}
              />
            </div>

            {formData.autoscale?.enabled && (
              <div className="space-y-4 pl-4 border-l-2 border-muted">
                <div className="space-y-2">
                  <div className="flex justify-between text-sm">
                    <span>Minimum Compute</span>
                    <span className="font-medium">
                      {formData.autoscale?.minComputeUnits ?? 0.25} CU
                      ({COMPUTE_UNITS.find(
                        (u) => u.value === (formData.autoscale?.minComputeUnits ?? 0.25)
                      )?.ram ?? "1 GB"} RAM)
                    </span>
                  </div>
                  <Slider
                    value={[COMPUTE_UNITS.findIndex(
                      (u) => u.value === (formData.autoscale?.minComputeUnits ?? 0.25)
                    )]}
                    onValueChange={(values) => {
                      const index = values[0];
                      if (index !== undefined && COMPUTE_UNITS[index]) {
                        handleAutoscaleChange("minComputeUnits", COMPUTE_UNITS[index].value);
                      }
                    }}
                    max={COMPUTE_UNITS.length - 1}
                    step={1}
                    disabled={isSaving}
                  />
                </div>

                <div className="space-y-2">
                  <div className="flex justify-between text-sm">
                    <span>Maximum Compute</span>
                    <span className="font-medium">
                      {formData.autoscale?.maxComputeUnits ?? 4} CU
                      ({COMPUTE_UNITS.find(
                        (u) => u.value === (formData.autoscale?.maxComputeUnits ?? 4)
                      )?.ram ?? "16 GB"} RAM)
                    </span>
                  </div>
                  <Slider
                    value={[COMPUTE_UNITS.findIndex(
                      (u) => u.value === (formData.autoscale?.maxComputeUnits ?? 4)
                    )]}
                    onValueChange={(values) => {
                      const index = values[0];
                      if (index !== undefined && COMPUTE_UNITS[index]) {
                        handleAutoscaleChange("maxComputeUnits", COMPUTE_UNITS[index].value);
                      }
                    }}
                    max={COMPUTE_UNITS.length - 1}
                    step={1}
                    disabled={isSaving}
                  />
                </div>

                <div className="flex items-center justify-between">
                  <div className="space-y-0.5">
                    <Label>Scale to Zero</Label>
                    <p className="text-sm text-muted-foreground">
                      Suspend compute when idle to save costs
                    </p>
                  </div>
                  <Switch
                    checked={formData.autoscale?.scaleToZero ?? false}
                    onCheckedChange={(checked) =>
                      handleAutoscaleChange("scaleToZero", checked)
                    }
                    disabled={isSaving}
                  />
                </div>

                {formData.autoscale?.scaleToZero && (
                  <div className="space-y-2">
                    <Label>Scale to Zero Delay</Label>
                    <Select
                      value={formData.autoscale?.scaleToZeroDelayMinutes?.toString() ?? "5"}
                      onValueChange={(value) =>
                        handleAutoscaleChange("scaleToZeroDelayMinutes", parseInt(value, 10))
                      }
                      disabled={isSaving}
                    >
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        {[1, 5, 10, 15, 30, 60].map((min) => (
                          <SelectItem key={min} value={min.toString()}>
                            {min} {min === 1 ? "minute" : "minutes"}
                          </SelectItem>
                        ))}
                      </SelectContent>
                    </Select>
                    <p className="text-xs text-muted-foreground">
                      Time before suspending compute after last activity
                    </p>
                  </div>
                )}
              </div>
            )}
          </div>
        </CardContent>
      </Card>

      {/* Connection Pooling */}
      <Card>
        <CardHeader>
          <div className="flex items-center gap-2">
            <Network className="h-5 w-5 text-primary" />
            <CardTitle>Connection Pooling</CardTitle>
          </div>
          <CardDescription>
            Configure PgBouncer for efficient connection management
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>Enable Connection Pooling</Label>
              <p className="text-sm text-muted-foreground">
                Use PgBouncer to manage database connections efficiently
              </p>
            </div>
            <Switch
              checked={formData.pooler?.enabled ?? false}
              onCheckedChange={(checked) =>
                handlePoolerChange("enabled", checked)
              }
              disabled={isSaving}
            />
          </div>

          {formData.pooler?.enabled && (
            <div className="space-y-4 pl-4 border-l-2 border-muted">
              <div className="space-y-2">
                <Label>Pooling Mode</Label>
                <Select
                  value={formData.pooler?.mode ?? "transaction"}
                  onValueChange={(value) =>
                    handlePoolerChange("mode", value as PoolerMode)
                  }
                  disabled={isSaving}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="transaction">
                      <div>
                        <span className="font-medium">Transaction</span>
                        <span className="text-muted-foreground ml-2">
                          (Recommended)
                        </span>
                      </div>
                    </SelectItem>
                    <SelectItem value="session">Session</SelectItem>
                  </SelectContent>
                </Select>
                <p className="text-xs text-muted-foreground">
                  Transaction mode releases connections after each transaction.
                  Session mode keeps connections for the entire session.
                </p>
              </div>

              <div className="grid grid-cols-2 gap-4">
                <div className="space-y-2">
                  <Label>Max Client Connections</Label>
                  <Input
                    type="number"
                    value={formData.pooler?.maxClientConnections ?? 200}
                    onChange={(e) =>
                      handlePoolerChange("maxClientConnections", parseInt(e.target.value, 10))
                    }
                    disabled={isSaving}
                    min={10}
                    max={10000}
                  />
                </div>
                <div className="space-y-2">
                  <Label>Default Pool Size</Label>
                  <Input
                    type="number"
                    value={formData.pooler?.defaultPoolSize ?? 15}
                    onChange={(e) =>
                      handlePoolerChange("defaultPoolSize", parseInt(e.target.value, 10))
                    }
                    disabled={isSaving}
                    min={1}
                    max={100}
                  />
                </div>
              </div>

              <div className="grid grid-cols-2 gap-4">
                <div className="space-y-2">
                  <Label>Max Pool Size</Label>
                  <Input
                    type="number"
                    value={formData.pooler?.maxPoolSize ?? 50}
                    onChange={(e) =>
                      handlePoolerChange("maxPoolSize", parseInt(e.target.value, 10))
                    }
                    disabled={isSaving}
                    min={1}
                    max={500}
                  />
                </div>
                <div className="space-y-2">
                  <Label>Idle Timeout (seconds)</Label>
                  <Input
                    type="number"
                    value={formData.pooler?.idleTimeout ?? 300}
                    onChange={(e) =>
                      handlePoolerChange("idleTimeout", parseInt(e.target.value, 10))
                    }
                    disabled={isSaving}
                    min={0}
                    max={86400}
                  />
                </div>
              </div>

              {cluster.endpoints?.pooler && (
                <Alert>
                  <Network className="h-4 w-4" />
                  <AlertDescription>
                    <span className="font-medium">Pooler endpoint: </span>
                    <code className="text-sm bg-muted px-1 py-0.5 rounded">
                      {cluster.endpoints.pooler}
                    </code>
                  </AlertDescription>
                </Alert>
              )}
            </div>
          )}
        </CardContent>
      </Card>

      {/* Network & Security */}
      <Card>
        <CardHeader>
          <div className="flex items-center gap-2">
            <Shield className="h-5 w-5 text-primary" />
            <CardTitle>Network & Security</CardTitle>
          </div>
          <CardDescription>
            Configure IP allow list and network security settings
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>Require SSL</Label>
              <p className="text-sm text-muted-foreground">
                Enforce SSL/TLS for all connections
              </p>
            </div>
            <Switch
              checked={formData.network?.sslRequired ?? true}
              onCheckedChange={(checked) =>
                handleNetworkChange("sslRequired", checked)
              }
              disabled={isSaving}
            />
          </div>

          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>Public Access</Label>
              <p className="text-sm text-muted-foreground">
                Allow connections from the public internet
              </p>
            </div>
            <Switch
              checked={formData.network?.publicAccess ?? true}
              onCheckedChange={(checked) =>
                handleNetworkChange("publicAccess", checked)
              }
              disabled={isSaving}
            />
          </div>

          <Separator />

          {/* IP Allow List */}
          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <div>
                <Label>IP Allow List</Label>
                <p className="text-sm text-muted-foreground">
                  Restrict access to specific IP addresses or CIDR ranges
                </p>
              </div>
              <Button
                variant="outline"
                size="sm"
                onClick={() => setShowAddIpDialog(true)}
                disabled={isSaving}
              >
                <Plus className="h-4 w-4 mr-2" />
                Add IP
              </Button>
            </div>

            {(formData.network?.ipAllowList?.length ?? 0) === 0 ? (
              <div className="text-center py-6 text-muted-foreground border rounded-lg border-dashed">
                <Globe className="h-8 w-8 mx-auto mb-2 opacity-50" />
                <p>No IP restrictions configured</p>
                <p className="text-sm">All IP addresses are allowed to connect</p>
              </div>
            ) : (
              <div className="space-y-2">
                {formData.network?.ipAllowList?.map((entry) => (
                  <div
                    key={entry.id}
                    className="flex items-center justify-between p-3 bg-muted/50 rounded-lg"
                  >
                    <div>
                      <code className="text-sm font-medium">{entry.cidr}</code>
                      {entry.description && (
                        <p className="text-sm text-muted-foreground">
                          {entry.description}
                        </p>
                      )}
                    </div>
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <Button
                          variant="ghost"
                          size="icon"
                          onClick={() => handleRemoveIp(entry.id)}
                          disabled={isSaving}
                        >
                          <X className="h-4 w-4" />
                        </Button>
                      </TooltipTrigger>
                      <TooltipContent>Remove IP</TooltipContent>
                    </Tooltip>
                  </div>
                ))}
              </div>
            )}
          </div>
        </CardContent>
      </Card>

      {/* Read Replicas */}
      <Card>
        <CardHeader>
          <div className="flex items-center gap-2">
            <Server className="h-5 w-5 text-primary" />
            <CardTitle>Read Replicas</CardTitle>
          </div>
          <CardDescription>
            Configure read replicas for improved read performance and availability
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>Enable Read Replicas</Label>
              <p className="text-sm text-muted-foreground">
                Create read-only replicas for scaling read workloads
              </p>
            </div>
            <Switch
              checked={formData.readReplicas?.enabled ?? false}
              onCheckedChange={(checked) =>
                handleReadReplicasChange("enabled", checked)
              }
              disabled={isSaving || isFree}
            />
          </div>

          {isFree && (
            <Alert>
              <AlertCircle className="h-4 w-4" />
              <AlertDescription>
                Upgrade to a paid plan to enable read replicas.
              </AlertDescription>
            </Alert>
          )}

          {formData.readReplicas?.enabled && !isFree && (
            <div className="space-y-4 pl-4 border-l-2 border-muted">
              <div className="space-y-2">
                <Label>Number of Replicas</Label>
                <Select
                  value={formData.readReplicas?.replicaCount?.toString() ?? "1"}
                  onValueChange={(value) =>
                    handleReadReplicasChange("replicaCount", parseInt(value, 10))
                  }
                  disabled={isSaving}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {[1, 2, 3, 4, 5].map((count) => (
                      <SelectItem key={count} value={count.toString()}>
                        {count} {count === 1 ? "replica" : "replicas"}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-2">
                <Label>Replica Regions</Label>
                <div className="flex flex-wrap gap-2">
                  {(formData.readReplicas?.regions ?? []).map((region) => (
                    <Badge key={region} variant="secondary">
                      {region}
                      <button
                        className="ml-1 hover:text-destructive"
                        onClick={() =>
                          handleReadReplicasChange(
                            "regions",
                            (formData.readReplicas?.regions ?? []).filter(
                              (r) => r !== region
                            )
                          )
                        }
                      >
                        <X className="h-3 w-3" />
                      </button>
                    </Badge>
                  ))}
                </div>
                <p className="text-xs text-muted-foreground">
                  Replicas will be created in the same region as the primary by default.
                </p>
              </div>

              {cluster.endpoints?.replica && (
                <Alert>
                  <Server className="h-4 w-4" />
                  <AlertDescription>
                    <span className="font-medium">Replica endpoint: </span>
                    <code className="text-sm bg-muted px-1 py-0.5 rounded">
                      {cluster.endpoints.replica}
                    </code>
                  </AlertDescription>
                </Alert>
              )}
            </div>
          )}
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
              disabled={isSaving || isFree}
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
      <div className="flex justify-end gap-4 sticky bottom-4 bg-background/80 backdrop-blur-sm p-4 -mx-4 rounded-lg border">
        <Button variant="outline" asChild>
          <Link to="/clusters/$id" params={{ id: clusterId }}>Cancel</Link>
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
      <DeleteClusterDialog
        open={showDeleteDialog}
        onOpenChange={setShowDeleteDialog}
        clusterName={cluster.name}
        onDelete={handleDelete}
        isDeleting={isSaving}
      />

      {/* Add IP Dialog */}
      <AddIpDialog
        open={showAddIpDialog}
        onOpenChange={setShowAddIpDialog}
        onAdd={handleAddIp}
      />
    </div>
  );
}
