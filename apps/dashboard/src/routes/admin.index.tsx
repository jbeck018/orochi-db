import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Users,
  Database,
  Building2,
  Activity,
  TrendingUp,
  ArrowRight,
  RefreshCw,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { useAdminStats } from "@/hooks/api";
import { cn } from "@/lib/utils";

export const Route = createFileRoute("/admin/")({
  component: AdminDashboardPage,
});

function AdminDashboardPage(): React.JSX.Element {
  const { data, isLoading, isRefetching, refetch } = useAdminStats();
  const stats = data?.data;

  const statCards = [
    {
      title: "Total Users",
      value: stats?.totalUsers ?? 0,
      description: `${stats?.activeUsers ?? 0} active`,
      icon: Users,
      href: "/admin/users",
      color: "text-blue-500",
    },
    {
      title: "Total Clusters",
      value: stats?.totalClusters ?? 0,
      description: `${stats?.runningClusters ?? 0} running`,
      icon: Database,
      href: "/admin/clusters",
      color: "text-green-500",
    },
    {
      title: "Organizations",
      value: stats?.totalOrganizations ?? 0,
      description: "Active teams",
      icon: Building2,
      href: "/admin/organizations",
      color: "text-purple-500",
    },
  ];

  return (
    <DashboardLayout>
      <div className="space-y-6">
        {/* Header */}
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-3xl font-bold tracking-tight">Admin Dashboard</h1>
            <p className="text-muted-foreground">
              Platform-wide management and monitoring
            </p>
          </div>
          <Button
            variant="outline"
            onClick={() => refetch()}
            disabled={isRefetching}
          >
            <RefreshCw
              className={cn("mr-2 h-4 w-4", isRefetching && "animate-spin")}
            />
            Refresh
          </Button>
        </div>

        {/* Stats Grid */}
        <div className="grid gap-4 md:grid-cols-2 lg:grid-cols-3">
          {isLoading
            ? [1, 2, 3].map((i) => (
                <Card key={i}>
                  <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
                    <Skeleton className="h-4 w-24" />
                    <Skeleton className="h-4 w-4" />
                  </CardHeader>
                  <CardContent>
                    <Skeleton className="h-8 w-16 mb-1" />
                    <Skeleton className="h-3 w-20" />
                  </CardContent>
                </Card>
              ))
            : statCards.map((card) => (
                <Card key={card.title} className="hover:border-primary/50 transition-colors">
                  <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
                    <CardTitle className="text-sm font-medium">
                      {card.title}
                    </CardTitle>
                    <card.icon className={cn("h-4 w-4", card.color)} />
                  </CardHeader>
                  <CardContent>
                    <div className="text-2xl font-bold">{card.value.toLocaleString()}</div>
                    <p className="text-xs text-muted-foreground">
                      {card.description}
                    </p>
                    <Button
                      variant="link"
                      size="sm"
                      className="mt-2 h-auto p-0"
                      asChild
                    >
                      <Link to={card.href}>
                        View all <ArrowRight className="ml-1 h-3 w-3" />
                      </Link>
                    </Button>
                  </CardContent>
                </Card>
              ))}
        </div>

        {/* Quick Actions */}
        <Card>
          <CardHeader>
            <CardTitle>Quick Actions</CardTitle>
            <CardDescription>
              Common administrative tasks
            </CardDescription>
          </CardHeader>
          <CardContent className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
            <Button variant="outline" className="justify-start h-auto py-4" asChild>
              <Link to="/admin/users">
                <Users className="mr-2 h-5 w-5 text-blue-500" />
                <div className="text-left">
                  <div className="font-medium">Manage Users</div>
                  <div className="text-xs text-muted-foreground">
                    View and edit user accounts
                  </div>
                </div>
              </Link>
            </Button>
            <Button variant="outline" className="justify-start h-auto py-4" asChild>
              <Link to="/admin/clusters">
                <Database className="mr-2 h-5 w-5 text-green-500" />
                <div className="text-left">
                  <div className="font-medium">Manage Clusters</div>
                  <div className="text-xs text-muted-foreground">
                    Monitor and manage all clusters
                  </div>
                </div>
              </Link>
            </Button>
            <Button variant="outline" className="justify-start h-auto py-4" disabled>
              <Activity className="mr-2 h-5 w-5 text-orange-500" />
              <div className="text-left">
                <div className="font-medium">System Health</div>
                <div className="text-xs text-muted-foreground">
                  Coming soon
                </div>
              </div>
            </Button>
            <Button variant="outline" className="justify-start h-auto py-4" disabled>
              <TrendingUp className="mr-2 h-5 w-5 text-purple-500" />
              <div className="text-left">
                <div className="font-medium">Analytics</div>
                <div className="text-xs text-muted-foreground">
                  Coming soon
                </div>
              </div>
            </Button>
          </CardContent>
        </Card>

        {/* Last updated */}
        {stats?.updatedAt && (
          <p className="text-sm text-muted-foreground">
            Last updated: {new Date(stats.updatedAt).toLocaleString()}
          </p>
        )}
      </div>
    </DashboardLayout>
  );
}
