import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { AlertCircle } from "lucide-react";
import { Button } from "@/components/ui/button";
import { useCluster } from "@/hooks/api";
import { BranchesList } from "@/components/branches";

export const Route = createFileRoute("/clusters/$id/branches")({
  component: BranchesPage,
});

function BranchesPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();

  const { data: clusterData, isLoading } = useCluster(clusterId);
  const cluster = clusterData?.data;

  if (isLoading) {
    return (
      <div className="flex items-center justify-center py-12">
        <div className="animate-spin h-8 w-8 border-4 border-primary border-t-transparent rounded-full" />
      </div>
    );
  }

  if (!cluster) {
    return (
      <div className="flex flex-col items-center justify-center py-12">
        <AlertCircle className="h-12 w-12 text-muted-foreground mb-4" />
        <p className="text-muted-foreground">Cluster not found</p>
      </div>
    );
  }

  return (
    <BranchesList clusterId={clusterId} clusterName={cluster.name} />
  );
}
