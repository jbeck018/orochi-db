"use client";

import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { DataBrowser } from "@/components/data-browser";

export const Route = createFileRoute("/clusters/$id/data")({
  component: DataBrowserPage,
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Browse and query your database tables",
      },
    ],
    title: "Data Browser - Orochi Cloud",
  }),
});

function DataBrowserPage(): React.JSX.Element {
  const { id: clusterId } = Route.useParams();

  return (
    <div className="h-[calc(100vh-300px)] min-h-[500px] rounded-lg border bg-background">
      <DataBrowser clusterId={clusterId} />
    </div>
  );
}
