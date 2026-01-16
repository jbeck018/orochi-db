"use client";

import * as React from "react";
import { useRouter } from "next/navigation";
import { Loader2, Check } from "lucide-react";
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
import { Switch } from "@/components/ui/switch";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { useToast } from "@/hooks/use-toast";
import { clusterApi } from "@/lib/api";
import { cn } from "@/lib/utils";
import {
  PROVIDERS,
  TIER_OPTIONS,
  type CloudProvider,
  type ClusterTier,
  type CreateClusterForm,
} from "@/types";

export function ClusterForm(): React.JSX.Element {
  const router = useRouter();
  const { toast } = useToast();
  const [isLoading, setIsLoading] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);

  const [formData, setFormData] = React.useState<CreateClusterForm>({
    name: "",
    tier: "standard",
    provider: "aws",
    region: "us-east-1",
    nodeCount: 1,
    storageGb: 50,
    highAvailability: false,
    backupEnabled: true,
  });

  const selectedProvider = PROVIDERS.find((p) => p.id === formData.provider);
  const selectedTier = TIER_OPTIONS.find((t) => t.id === formData.tier);

  const handleSubmit = async (e: React.FormEvent): Promise<void> => {
    e.preventDefault();
    setError(null);
    setIsLoading(true);

    try {
      const response = await clusterApi.create(formData);
      toast({
        title: "Cluster created",
        description: `${formData.name} is being provisioned`,
      });
      router.push(`/clusters/${response.data.id}`);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to create cluster");
    } finally {
      setIsLoading(false);
    }
  };

  return (
    <form onSubmit={handleSubmit} className="space-y-8">
      {error && (
        <Alert variant="destructive">
          <AlertDescription>{error}</AlertDescription>
        </Alert>
      )}

      {/* Basic Information */}
      <Card>
        <CardHeader>
          <CardTitle>Basic Information</CardTitle>
          <CardDescription>
            Choose a name and configuration for your cluster
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          <div className="space-y-2">
            <Label htmlFor="name">Cluster Name</Label>
            <Input
              id="name"
              placeholder="my-production-db"
              value={formData.name}
              onChange={(e) =>
                setFormData({ ...formData, name: e.target.value })
              }
              required
              disabled={isLoading}
            />
            <p className="text-xs text-muted-foreground">
              Use lowercase letters, numbers, and hyphens only
            </p>
          </div>
        </CardContent>
      </Card>

      {/* Tier Selection */}
      <Card>
        <CardHeader>
          <CardTitle>Select Plan</CardTitle>
          <CardDescription>
            Choose the plan that best fits your needs
          </CardDescription>
        </CardHeader>
        <CardContent>
          <div className="grid grid-cols-1 gap-4 md:grid-cols-2 lg:grid-cols-4">
            {TIER_OPTIONS.map((tier) => (
              <div
                key={tier.id}
                className={cn(
                  "relative flex cursor-pointer flex-col rounded-lg border-2 p-4 transition-all hover:border-primary/50",
                  formData.tier === tier.id
                    ? "border-primary bg-primary/5"
                    : "border-border"
                )}
                onClick={() => setFormData({ ...formData, tier: tier.id })}
              >
                {formData.tier === tier.id && (
                  <div className="absolute right-2 top-2">
                    <Check className="h-5 w-5 text-primary" />
                  </div>
                )}
                <h3 className="font-semibold">{tier.name}</h3>
                <p className="text-2xl font-bold text-primary">{tier.price}</p>
                <p className="text-sm text-muted-foreground">
                  {tier.description}
                </p>
                <ul className="mt-4 space-y-1 text-sm">
                  {tier.features.map((feature, index) => (
                    <li key={index} className="flex items-center gap-2">
                      <Check className="h-3 w-3 text-green-500" />
                      {feature}
                    </li>
                  ))}
                </ul>
              </div>
            ))}
          </div>
        </CardContent>
      </Card>

      {/* Cloud Provider & Region */}
      <Card>
        <CardHeader>
          <CardTitle>Cloud Provider & Region</CardTitle>
          <CardDescription>
            Select your preferred cloud provider and region
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
            <div className="space-y-2">
              <Label>Cloud Provider</Label>
              <Select
                value={formData.provider}
                onValueChange={(value: CloudProvider) =>
                  setFormData({
                    ...formData,
                    provider: value,
                    region: PROVIDERS.find((p) => p.id === value)?.regions[0]?.id ?? "",
                  })
                }
                disabled={isLoading}
              >
                <SelectTrigger>
                  <SelectValue placeholder="Select provider" />
                </SelectTrigger>
                <SelectContent>
                  {PROVIDERS.map((provider) => (
                    <SelectItem key={provider.id} value={provider.id}>
                      {provider.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <div className="space-y-2">
              <Label>Region</Label>
              <Select
                value={formData.region}
                onValueChange={(value) =>
                  setFormData({ ...formData, region: value })
                }
                disabled={isLoading}
              >
                <SelectTrigger>
                  <SelectValue placeholder="Select region" />
                </SelectTrigger>
                <SelectContent>
                  {selectedProvider?.regions.map((region) => (
                    <SelectItem key={region.id} value={region.id}>
                      {region.name} ({region.location})
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
          </div>
        </CardContent>
      </Card>

      {/* Resources */}
      <Card>
        <CardHeader>
          <CardTitle>Resources</CardTitle>
          <CardDescription>
            Configure compute and storage resources
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
            <div className="space-y-2">
              <Label htmlFor="nodeCount">Node Count</Label>
              <Select
                value={formData.nodeCount.toString()}
                onValueChange={(value) =>
                  setFormData({ ...formData, nodeCount: parseInt(value, 10) })
                }
                disabled={isLoading || formData.tier === "free"}
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
            </div>
            <div className="space-y-2">
              <Label htmlFor="storage">Storage (GB)</Label>
              <Select
                value={formData.storageGb.toString()}
                onValueChange={(value) =>
                  setFormData({ ...formData, storageGb: parseInt(value, 10) })
                }
                disabled={isLoading || formData.tier === "free"}
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
        </CardContent>
      </Card>

      {/* Features */}
      <Card>
        <CardHeader>
          <CardTitle>Features</CardTitle>
          <CardDescription>
            Enable additional features for your cluster
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>High Availability</Label>
              <p className="text-sm text-muted-foreground">
                Deploy across multiple availability zones for redundancy
              </p>
            </div>
            <Switch
              checked={formData.highAvailability}
              onCheckedChange={(checked) =>
                setFormData({ ...formData, highAvailability: checked })
              }
              disabled={isLoading || formData.tier === "free" || formData.tier === "standard"}
            />
          </div>
          <div className="flex items-center justify-between">
            <div className="space-y-0.5">
              <Label>Automatic Backups</Label>
              <p className="text-sm text-muted-foreground">
                Enable daily automated backups with point-in-time recovery
              </p>
            </div>
            <Switch
              checked={formData.backupEnabled}
              onCheckedChange={(checked) =>
                setFormData({ ...formData, backupEnabled: checked })
              }
              disabled={isLoading || formData.tier === "free"}
            />
          </div>
        </CardContent>
      </Card>

      {/* Summary */}
      <Card>
        <CardHeader>
          <CardTitle>Summary</CardTitle>
        </CardHeader>
        <CardContent>
          <div className="space-y-2 text-sm">
            <div className="flex justify-between">
              <span className="text-muted-foreground">Plan</span>
              <span className="font-medium">{selectedTier?.name}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-muted-foreground">Provider</span>
              <span className="font-medium">{selectedProvider?.name}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-muted-foreground">Region</span>
              <span className="font-medium">{formData.region}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-muted-foreground">Nodes</span>
              <span className="font-medium">{formData.nodeCount}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-muted-foreground">Storage</span>
              <span className="font-medium">{formData.storageGb} GB</span>
            </div>
            <div className="border-t pt-2 mt-2">
              <div className="flex justify-between text-base font-semibold">
                <span>Estimated Monthly Cost</span>
                <span className="text-primary">{selectedTier?.price}</span>
              </div>
            </div>
          </div>
        </CardContent>
      </Card>

      {/* Actions */}
      <div className="flex justify-end gap-4">
        <Button
          type="button"
          variant="outline"
          onClick={() => router.back()}
          disabled={isLoading}
        >
          Cancel
        </Button>
        <Button type="submit" disabled={isLoading || !formData.name}>
          {isLoading ? (
            <>
              <Loader2 className="mr-2 h-4 w-4 animate-spin" />
              Creating...
            </>
          ) : (
            "Create Cluster"
          )}
        </Button>
      </div>
    </form>
  );
}
