"use client";

import * as React from "react";
import { Save, RefreshCw, AlertCircle, Info } from "lucide-react";
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Switch } from "@/components/ui/switch";
import { Slider } from "@/components/ui/slider";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { Skeleton } from "@/components/ui/skeleton";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { Separator } from "@/components/ui/separator";
import { useToast } from "@/hooks/use-toast";
import {
  usePoolerConfig,
  useUpdatePoolerConfig,
  useReloadPoolerConfig,
} from "@/hooks/api";
import type { PoolerConfig, PoolerMode, UpdatePoolerConfigRequest } from "@/types/pooler";

interface PoolerConfigPanelProps {
  clusterId: string;
}

interface ConfigFormState {
  enabled: boolean;
  mode: PoolerMode;
  maxPoolSize: number;
  minPoolSize: number;
  idleTimeoutSeconds: number;
  readWriteSplit: boolean;
  shardingEnabled: boolean;
  shardCount: number;
  loadBalancingMode: "round_robin" | "least_connections" | "random";
  healthCheckIntervalSeconds: number;
  connectionTimeoutMs: number;
  queryTimeoutMs: number;
}

function configToFormState(config: PoolerConfig): ConfigFormState {
  return {
    enabled: config.enabled,
    mode: config.mode,
    maxPoolSize: config.maxPoolSize,
    minPoolSize: config.minPoolSize,
    idleTimeoutSeconds: config.idleTimeoutSeconds,
    readWriteSplit: config.readWriteSplit,
    shardingEnabled: config.shardingEnabled,
    shardCount: config.shardCount ?? 1,
    loadBalancingMode: config.loadBalancingMode ?? "round_robin",
    healthCheckIntervalSeconds: config.healthCheckIntervalSeconds ?? 30,
    connectionTimeoutMs: config.connectionTimeoutMs ?? 5000,
    queryTimeoutMs: config.queryTimeoutMs ?? 30000,
  };
}

export const PoolerConfigPanel = React.memo(function PoolerConfigPanel({
  clusterId,
}: PoolerConfigPanelProps): React.JSX.Element {
  const { toast } = useToast();

  const { data: configData, isLoading, error } = usePoolerConfig(clusterId);
  const config = configData?.data;

  const updateMutation = useUpdatePoolerConfig(clusterId);
  const reloadMutation = useReloadPoolerConfig(clusterId);

  const [formState, setFormState] = React.useState<ConfigFormState | null>(null);
  const [hasChanges, setHasChanges] = React.useState(false);

  // Initialize form state when config loads
  React.useEffect(() => {
    if (config && !formState) {
      setFormState(configToFormState(config));
    }
  }, [config, formState]);

  // Check for changes when form state updates
  React.useEffect(() => {
    if (config && formState) {
      const original = configToFormState(config);
      const changed = JSON.stringify(original) !== JSON.stringify(formState);
      setHasChanges(changed);
    }
  }, [config, formState]);

  const handleChange = <K extends keyof ConfigFormState>(
    key: K,
    value: ConfigFormState[K]
  ): void => {
    if (formState) {
      setFormState((prev) => (prev ? { ...prev, [key]: value } : prev));
    }
  };

  const handleSave = async (): Promise<void> => {
    if (!formState) return;

    try {
      const updateRequest: UpdatePoolerConfigRequest = {
        enabled: formState.enabled,
        mode: formState.mode,
        maxPoolSize: formState.maxPoolSize,
        minPoolSize: formState.minPoolSize,
        idleTimeoutSeconds: formState.idleTimeoutSeconds,
        readWriteSplit: formState.readWriteSplit,
        shardingEnabled: formState.shardingEnabled,
        shardCount: formState.shardingEnabled ? formState.shardCount : undefined,
        loadBalancingMode: formState.loadBalancingMode,
        healthCheckIntervalSeconds: formState.healthCheckIntervalSeconds,
        connectionTimeoutMs: formState.connectionTimeoutMs,
        queryTimeoutMs: formState.queryTimeoutMs,
      };

      await updateMutation.mutateAsync(updateRequest);
      toast({
        title: "Configuration saved",
        description: "Pooler configuration has been updated. Click reload to apply changes.",
      });
    } catch (err) {
      toast({
        title: "Save failed",
        description: err instanceof Error ? err.message : "Failed to save configuration",
        variant: "destructive",
      });
    }
  };

  const handleReload = async (): Promise<void> => {
    try {
      await reloadMutation.mutateAsync();
      toast({
        title: "Configuration reloaded",
        description: "Pooler configuration has been applied without restart.",
      });
    } catch (err) {
      toast({
        title: "Reload failed",
        description: err instanceof Error ? err.message : "Failed to reload configuration",
        variant: "destructive",
      });
    }
  };

  const handleReset = (): void => {
    if (config) {
      setFormState(configToFormState(config));
    }
  };

  if (isLoading) {
    return (
      <Card>
        <CardHeader>
          <Skeleton className="h-6 w-48" />
          <Skeleton className="h-4 w-72" />
        </CardHeader>
        <CardContent className="space-y-6">
          {[1, 2, 3, 4].map((i) => (
            <div key={i} className="space-y-2">
              <Skeleton className="h-4 w-32" />
              <Skeleton className="h-10 w-full" />
            </div>
          ))}
        </CardContent>
      </Card>
    );
  }

  if (error || !config || !formState) {
    return (
      <Card>
        <CardHeader>
          <CardTitle>Pooler Configuration</CardTitle>
        </CardHeader>
        <CardContent>
          <Alert variant="destructive">
            <AlertCircle className="h-4 w-4" />
            <AlertTitle>Error</AlertTitle>
            <AlertDescription>
              Unable to load pooler configuration. Please try again later.
            </AlertDescription>
          </Alert>
        </CardContent>
      </Card>
    );
  }

  return (
    <TooltipProvider>
      <Card>
        <CardHeader>
          <CardTitle>Pooler Configuration</CardTitle>
          <CardDescription>
            Configure PgDog connection pooler settings for your cluster
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          {/* Enable/Disable */}
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label htmlFor="enabled">Enable Connection Pooler</Label>
              <p className="text-sm text-muted-foreground">
                Enable PgDog connection pooling for this cluster
              </p>
            </div>
            <Switch
              id="enabled"
              checked={formState.enabled}
              onCheckedChange={(checked) => handleChange("enabled", checked)}
            />
          </div>

          <Separator />

          {/* Pooling Mode */}
          <div className="space-y-2">
            <div className="flex items-center gap-2">
              <Label htmlFor="mode">Pooling Mode</Label>
              <Tooltip>
                <TooltipTrigger>
                  <Info className="h-4 w-4 text-muted-foreground" />
                </TooltipTrigger>
                <TooltipContent className="max-w-sm">
                  <p><strong>Transaction:</strong> Connection returned to pool after each transaction (recommended)</p>
                  <p><strong>Session:</strong> Connection held for entire client session</p>
                  <p><strong>Statement:</strong> Connection returned after each statement</p>
                </TooltipContent>
              </Tooltip>
            </div>
            <Select
              value={formState.mode}
              onValueChange={(value) => handleChange("mode", value as PoolerMode)}
              disabled={!formState.enabled}
            >
              <SelectTrigger id="mode">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="transaction">Transaction Pooling</SelectItem>
                <SelectItem value="session">Session Pooling</SelectItem>
                <SelectItem value="statement">Statement Pooling</SelectItem>
              </SelectContent>
            </Select>
          </div>

          {/* Pool Size */}
          <div className="space-y-4">
            <div className="space-y-2">
              <div className="flex items-center justify-between">
                <Label>Pool Size Range</Label>
                <span className="text-sm font-mono">
                  {formState.minPoolSize} - {formState.maxPoolSize}
                </span>
              </div>
              <div className="grid grid-cols-2 gap-4">
                <div className="space-y-2">
                  <Label htmlFor="minPoolSize" className="text-xs text-muted-foreground">
                    Minimum
                  </Label>
                  <Input
                    id="minPoolSize"
                    type="number"
                    min={1}
                    max={formState.maxPoolSize}
                    value={formState.minPoolSize}
                    onChange={(e) =>
                      handleChange("minPoolSize", parseInt(e.target.value) || 1)
                    }
                    disabled={!formState.enabled}
                  />
                </div>
                <div className="space-y-2">
                  <Label htmlFor="maxPoolSize" className="text-xs text-muted-foreground">
                    Maximum
                  </Label>
                  <Input
                    id="maxPoolSize"
                    type="number"
                    min={formState.minPoolSize}
                    max={1000}
                    value={formState.maxPoolSize}
                    onChange={(e) =>
                      handleChange("maxPoolSize", parseInt(e.target.value) || 100)
                    }
                    disabled={!formState.enabled}
                  />
                </div>
              </div>
            </div>
          </div>

          {/* Idle Timeout */}
          <div className="space-y-2">
            <div className="flex items-center justify-between">
              <Label htmlFor="idleTimeoutSeconds">Idle Timeout (seconds)</Label>
              <span className="text-sm font-mono">{formState.idleTimeoutSeconds}s</span>
            </div>
            <Slider
              id="idleTimeoutSeconds"
              min={10}
              max={3600}
              step={10}
              value={[formState.idleTimeoutSeconds]}
              onValueChange={([value]) => handleChange("idleTimeoutSeconds", value ?? formState.idleTimeoutSeconds)}
              disabled={!formState.enabled}
            />
            <p className="text-xs text-muted-foreground">
              Close idle server connections after this time
            </p>
          </div>

          <Separator />

          {/* Read/Write Split */}
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label htmlFor="readWriteSplit">Read/Write Splitting</Label>
              <p className="text-sm text-muted-foreground">
                Route read queries to replicas automatically
              </p>
            </div>
            <Switch
              id="readWriteSplit"
              checked={formState.readWriteSplit}
              onCheckedChange={(checked) => handleChange("readWriteSplit", checked)}
              disabled={!formState.enabled}
            />
          </div>

          {/* Load Balancing Mode */}
          <div className="space-y-2">
            <Label htmlFor="loadBalancingMode">Load Balancing Mode</Label>
            <Select
              value={formState.loadBalancingMode}
              onValueChange={(value) =>
                handleChange(
                  "loadBalancingMode",
                  value as "round_robin" | "least_connections" | "random"
                )
              }
              disabled={!formState.enabled}
            >
              <SelectTrigger id="loadBalancingMode">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="round_robin">Round Robin</SelectItem>
                <SelectItem value="least_connections">Least Connections</SelectItem>
                <SelectItem value="random">Random</SelectItem>
              </SelectContent>
            </Select>
          </div>

          <Separator />

          {/* Sharding */}
          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <div className="space-y-0.5">
                <Label htmlFor="shardingEnabled">Sharding Support</Label>
                <p className="text-sm text-muted-foreground">
                  Enable query routing to shards
                </p>
              </div>
              <Switch
                id="shardingEnabled"
                checked={formState.shardingEnabled}
                onCheckedChange={(checked) => handleChange("shardingEnabled", checked)}
                disabled={!formState.enabled}
              />
            </div>

            {formState.shardingEnabled && (
              <div className="space-y-2">
                <Label htmlFor="shardCount">Shard Count</Label>
                <Input
                  id="shardCount"
                  type="number"
                  min={1}
                  max={256}
                  value={formState.shardCount}
                  onChange={(e) =>
                    handleChange("shardCount", parseInt(e.target.value) || 1)
                  }
                  disabled={!formState.enabled}
                />
              </div>
            )}
          </div>

          <Separator />

          {/* Advanced Settings */}
          <div className="space-y-4">
            <h4 className="text-sm font-medium">Advanced Settings</h4>

            <div className="grid grid-cols-2 gap-4">
              <div className="space-y-2">
                <div className="flex items-center gap-2">
                  <Label htmlFor="connectionTimeoutMs">Connection Timeout (ms)</Label>
                  <Tooltip>
                    <TooltipTrigger>
                      <Info className="h-4 w-4 text-muted-foreground" />
                    </TooltipTrigger>
                    <TooltipContent>
                      Maximum time to wait for a server connection
                    </TooltipContent>
                  </Tooltip>
                </div>
                <Input
                  id="connectionTimeoutMs"
                  type="number"
                  min={100}
                  max={60000}
                  value={formState.connectionTimeoutMs}
                  onChange={(e) =>
                    handleChange("connectionTimeoutMs", parseInt(e.target.value) || 5000)
                  }
                  disabled={!formState.enabled}
                />
              </div>

              <div className="space-y-2">
                <div className="flex items-center gap-2">
                  <Label htmlFor="queryTimeoutMs">Query Timeout (ms)</Label>
                  <Tooltip>
                    <TooltipTrigger>
                      <Info className="h-4 w-4 text-muted-foreground" />
                    </TooltipTrigger>
                    <TooltipContent>
                      Maximum time for a query to complete (0 = unlimited)
                    </TooltipContent>
                  </Tooltip>
                </div>
                <Input
                  id="queryTimeoutMs"
                  type="number"
                  min={0}
                  max={300000}
                  value={formState.queryTimeoutMs}
                  onChange={(e) =>
                    handleChange("queryTimeoutMs", parseInt(e.target.value) || 30000)
                  }
                  disabled={!formState.enabled}
                />
              </div>
            </div>

            <div className="space-y-2">
              <Label htmlFor="healthCheckIntervalSeconds">Health Check Interval (seconds)</Label>
              <Slider
                id="healthCheckIntervalSeconds"
                min={5}
                max={300}
                step={5}
                value={[formState.healthCheckIntervalSeconds]}
                onValueChange={([value]) =>
                  handleChange("healthCheckIntervalSeconds", value ?? formState.healthCheckIntervalSeconds)
                }
                disabled={!formState.enabled}
              />
              <div className="flex justify-between text-xs text-muted-foreground">
                <span>5s (aggressive)</span>
                <span className="font-mono">{formState.healthCheckIntervalSeconds}s</span>
                <span>300s (relaxed)</span>
              </div>
            </div>
          </div>
        </CardContent>
        <CardFooter className="flex justify-between">
          <div className="flex gap-2">
            <Button
              variant="outline"
              onClick={handleReset}
              disabled={!hasChanges || updateMutation.isPending}
            >
              Reset
            </Button>
          </div>
          <div className="flex gap-2">
            <Button
              variant="outline"
              onClick={handleReload}
              disabled={reloadMutation.isPending}
            >
              <RefreshCw
                className={`mr-2 h-4 w-4 ${reloadMutation.isPending ? "animate-spin" : ""}`}
              />
              Reload
            </Button>
            <Button
              onClick={handleSave}
              disabled={!hasChanges || updateMutation.isPending}
            >
              <Save className="mr-2 h-4 w-4" />
              {updateMutation.isPending ? "Saving..." : "Save Changes"}
            </Button>
          </div>
        </CardFooter>
      </Card>
    </TooltipProvider>
  );
});
