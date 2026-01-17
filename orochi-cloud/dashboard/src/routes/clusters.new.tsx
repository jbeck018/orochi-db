import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { ClusterForm } from "@/components/clusters/cluster-form";

export const Route = createFileRoute("/clusters/new")({
  component: NewClusterPage,
});

function NewClusterPage(): React.JSX.Element {
  return (
    <DashboardLayout>
      <div className="max-w-4xl mx-auto space-y-6">
        <div>
          <h1 className="text-3xl font-bold tracking-tight">Create Cluster</h1>
          <p className="text-muted-foreground">
            Deploy a new PostgreSQL HTAP cluster with Orochi DB
          </p>
        </div>

        <ClusterForm />
      </div>
    </DashboardLayout>
  );
}
