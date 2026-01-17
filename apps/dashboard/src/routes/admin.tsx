import * as React from "react";
import { createFileRoute, Outlet, redirect } from "@tanstack/react-router";
import { requireAuth } from "@/lib/auth";

export const Route = createFileRoute("/admin")({
  beforeLoad: () => {
    const { isAuthenticated, isAdmin } = requireAuth();

    if (!isAuthenticated) {
      throw redirect({
        to: "/login",
        search: {
          redirect: "/admin",
        },
      });
    }

    if (!isAdmin) {
      throw redirect({
        to: "/clusters",
      });
    }
  },
  component: AdminLayout,
});

function AdminLayout(): React.JSX.Element {
  return <Outlet />;
}
