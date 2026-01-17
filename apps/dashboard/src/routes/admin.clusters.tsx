import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Search,
  MoreHorizontal,
  Trash2,
  Eye,
  Filter,
  ChevronLeft,
  ChevronRight,
  RefreshCw,
  ArrowLeft,
  Server,
  HardDrive,
  Clock,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from "@/components/ui/alert-dialog";
import { useAdminClusters, useForceDeleteCluster } from "@/hooks/api";
import { useToast } from "@/hooks/use-toast";
import { cn } from "@/lib/utils";
import type { AdminCluster, ClusterStatus } from "@/types";

export const Route = createFileRoute("/admin/clusters")({
  component: AdminClustersPage,
});

function AdminClustersPage(): React.JSX.Element {
  const [searchQuery, setSearchQuery] = React.useState("");
  const [statusFilter, setStatusFilter] = React.useState<ClusterStatus | "all">("all");
  const [page, setPage] = React.useState(1);
  const [deleteDialog, setDeleteDialog] = React.useState<{
    open: boolean;
    cluster?: AdminCluster;
  }>({ open: false });
  const { toast } = useToast();
  const pageSize = 20;

  const { data, isLoading, isRefetching, refetch } = useAdminClusters(
    page,
    pageSize,
    statusFilter === "all" ? undefined : statusFilter
  );
  const forceDeleteMutation = useForceDeleteCluster();

  const clusters = data?.clusters ?? [];
  const totalCount = data?.totalCount ?? 0;
  const totalPages = Math.ceil(totalCount / pageSize);

  // Filter by search locally
  const filteredClusters = React.useMemo(() => {
    if (!searchQuery) return clusters;
    const lowerQuery = searchQuery.toLowerCase();
    return clusters.filter(
      (cluster) =>
        cluster.name.toLowerCase().includes(lowerQuery) ||
        cluster.ownerEmail.toLowerCase().includes(lowerQuery) ||
        cluster.ownerName.toLowerCase().includes(lowerQuery)
    );
  }, [clusters, searchQuery]);

  const handleForceDelete = async (cluster: AdminCluster) => {
    try {
      await forceDeleteMutation.mutateAsync(cluster.id);
      toast({
        title: "Cluster deleted",
        description: `${cluster.name} has been permanently deleted`,
      });
      setDeleteDialog({ open: false });
    } catch (error) {
      toast({
        title: "Error",
        description: "Failed to delete cluster",
        variant: "destructive",
      });
    }
  };

  const getStatusBadge = (status: ClusterStatus) => {
    switch (status) {
      case "running":
        return (
          <Badge variant="outline" className="border-green-500 text-green-500">
            <span className="mr-1 h-2 w-2 rounded-full bg-green-500 inline-block animate-pulse" />
            Running
          </Badge>
        );
      case "stopped":
        return (
          <Badge variant="outline" className="border-gray-500 text-gray-500">
            Stopped
          </Badge>
        );
      case "creating":
        return (
          <Badge variant="outline" className="border-blue-500 text-blue-500">
            <span className="mr-1 h-2 w-2 rounded-full bg-blue-500 inline-block animate-pulse" />
            Creating
          </Badge>
        );
      case "error":
        return (
          <Badge variant="outline" className="border-red-500 text-red-500">
            Error
          </Badge>
        );
      case "deleting":
        return (
          <Badge variant="outline" className="border-orange-500 text-orange-500">
            Deleting
          </Badge>
        );
      case "updating":
        return (
          <Badge variant="outline" className="border-yellow-500 text-yellow-500">
            Updating
          </Badge>
        );
      default:
        return <Badge variant="outline">{status}</Badge>;
    }
  };

  const getTierBadge = (tier: string) => {
    switch (tier) {
      case "free":
        return <Badge variant="secondary">Free</Badge>;
      case "standard":
        return <Badge variant="outline">Standard</Badge>;
      case "professional":
        return <Badge className="bg-blue-500 hover:bg-blue-600">Professional</Badge>;
      case "enterprise":
        return <Badge className="bg-purple-500 hover:bg-purple-600">Enterprise</Badge>;
      default:
        return <Badge variant="outline">{tier}</Badge>;
    }
  };

  return (
    <DashboardLayout>
      <div className="space-y-6">
        {/* Header */}
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-4">
            <Button variant="ghost" size="icon" asChild>
              <Link to="/admin">
                <ArrowLeft className="h-4 w-4" />
              </Link>
            </Button>
            <div>
              <h1 className="text-3xl font-bold tracking-tight">Clusters</h1>
              <p className="text-muted-foreground">
                Manage all platform clusters
              </p>
            </div>
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

        {/* Filters */}
        <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
          <div className="flex flex-1 items-center gap-2">
            <div className="relative flex-1 max-w-sm">
              <Search className="absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
              <Input
                placeholder="Search clusters or owners..."
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                className="pl-9"
              />
            </div>
            <Select
              value={statusFilter}
              onValueChange={(value) => {
                setStatusFilter(value as ClusterStatus | "all");
                setPage(1);
              }}
            >
              <SelectTrigger className="w-32">
                <Filter className="mr-2 h-4 w-4" />
                <SelectValue placeholder="Status" />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="all">All</SelectItem>
                <SelectItem value="running">Running</SelectItem>
                <SelectItem value="stopped">Stopped</SelectItem>
                <SelectItem value="creating">Creating</SelectItem>
                <SelectItem value="error">Error</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <p className="text-sm text-muted-foreground">
            {totalCount} cluster{totalCount !== 1 ? "s" : ""} total
          </p>
        </div>

        {/* Clusters Table */}
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Database className="h-5 w-5" />
              All Clusters
            </CardTitle>
            <CardDescription>
              View and manage all clusters across the platform
            </CardDescription>
          </CardHeader>
          <CardContent>
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>Cluster</TableHead>
                  <TableHead>Owner</TableHead>
                  <TableHead>Status</TableHead>
                  <TableHead>Tier</TableHead>
                  <TableHead>Resources</TableHead>
                  <TableHead>Created</TableHead>
                  <TableHead className="w-[70px]"></TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {isLoading ? (
                  Array.from({ length: 5 }).map((_, i) => (
                    <TableRow key={i}>
                      <TableCell>
                        <div className="space-y-1">
                          <Skeleton className="h-4 w-32" />
                          <Skeleton className="h-3 w-24" />
                        </div>
                      </TableCell>
                      <TableCell>
                        <div className="space-y-1">
                          <Skeleton className="h-4 w-24" />
                          <Skeleton className="h-3 w-32" />
                        </div>
                      </TableCell>
                      <TableCell><Skeleton className="h-5 w-16" /></TableCell>
                      <TableCell><Skeleton className="h-5 w-14" /></TableCell>
                      <TableCell><Skeleton className="h-4 w-20" /></TableCell>
                      <TableCell><Skeleton className="h-4 w-24" /></TableCell>
                      <TableCell><Skeleton className="h-8 w-8" /></TableCell>
                    </TableRow>
                  ))
                ) : filteredClusters.length > 0 ? (
                  filteredClusters.map((cluster) => (
                    <TableRow key={cluster.id}>
                      <TableCell>
                        <div>
                          <div className="font-medium">{cluster.name}</div>
                          <div className="text-sm text-muted-foreground flex items-center gap-1">
                            {cluster.provider.toUpperCase()} / {cluster.region}
                          </div>
                        </div>
                      </TableCell>
                      <TableCell>
                        <div>
                          <div className="font-medium">{cluster.ownerName}</div>
                          <div className="text-sm text-muted-foreground">
                            {cluster.ownerEmail}
                          </div>
                        </div>
                      </TableCell>
                      <TableCell>{getStatusBadge(cluster.status)}</TableCell>
                      <TableCell>{getTierBadge(cluster.tier)}</TableCell>
                      <TableCell>
                        <div className="flex flex-col gap-1 text-sm text-muted-foreground">
                          <span className="flex items-center gap-1">
                            <Server className="h-3 w-3" />
                            {cluster.nodeCount} node{cluster.nodeCount !== 1 ? "s" : ""}
                          </span>
                          <span className="flex items-center gap-1">
                            <HardDrive className="h-3 w-3" />
                            {cluster.storageGb} GB
                          </span>
                        </div>
                      </TableCell>
                      <TableCell>
                        <div className="flex items-center gap-1 text-muted-foreground">
                          <Clock className="h-3 w-3" />
                          {new Date(cluster.createdAt).toLocaleDateString()}
                        </div>
                      </TableCell>
                      <TableCell>
                        <DropdownMenu>
                          <DropdownMenuTrigger asChild>
                            <Button variant="ghost" size="icon">
                              <MoreHorizontal className="h-4 w-4" />
                            </Button>
                          </DropdownMenuTrigger>
                          <DropdownMenuContent align="end">
                            <DropdownMenuLabel>Actions</DropdownMenuLabel>
                            <DropdownMenuSeparator />
                            <DropdownMenuItem asChild>
                              <Link to="/clusters/$id" params={{ id: cluster.id }}>
                                <Eye className="mr-2 h-4 w-4" />
                                View Details
                              </Link>
                            </DropdownMenuItem>
                            <DropdownMenuSeparator />
                            <DropdownMenuItem
                              onClick={() =>
                                setDeleteDialog({ open: true, cluster })
                              }
                              className="text-red-500"
                            >
                              <Trash2 className="mr-2 h-4 w-4" />
                              Force Delete
                            </DropdownMenuItem>
                          </DropdownMenuContent>
                        </DropdownMenu>
                      </TableCell>
                    </TableRow>
                  ))
                ) : (
                  <TableRow>
                    <TableCell colSpan={7} className="text-center py-10">
                      <Database className="h-12 w-12 mx-auto mb-4 text-muted-foreground" />
                      <p className="text-muted-foreground">
                        {searchQuery || statusFilter !== "all"
                          ? "No clusters found matching your criteria"
                          : "No clusters found"}
                      </p>
                    </TableCell>
                  </TableRow>
                )}
              </TableBody>
            </Table>

            {/* Pagination */}
            {totalPages > 1 && (
              <div className="flex items-center justify-between mt-4">
                <p className="text-sm text-muted-foreground">
                  Page {page} of {totalPages}
                </p>
                <div className="flex items-center gap-2">
                  <Button
                    variant="outline"
                    size="sm"
                    onClick={() => setPage((p) => Math.max(1, p - 1))}
                    disabled={page === 1}
                  >
                    <ChevronLeft className="h-4 w-4" />
                    Previous
                  </Button>
                  <Button
                    variant="outline"
                    size="sm"
                    onClick={() => setPage((p) => Math.min(totalPages, p + 1))}
                    disabled={page === totalPages}
                  >
                    Next
                    <ChevronRight className="h-4 w-4" />
                  </Button>
                </div>
              </div>
            )}
          </CardContent>
        </Card>
      </div>

      {/* Delete Confirmation Dialog */}
      <AlertDialog
        open={deleteDialog.open}
        onOpenChange={(open) => setDeleteDialog((prev) => ({ ...prev, open }))}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>Force Delete Cluster</AlertDialogTitle>
            <AlertDialogDescription>
              Are you sure you want to permanently delete{" "}
              <strong>{deleteDialog.cluster?.name}</strong>? This action cannot
              be undone and will delete all associated data.
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>Cancel</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                if (deleteDialog.cluster) {
                  handleForceDelete(deleteDialog.cluster);
                }
              }}
              className="bg-red-500 hover:bg-red-600"
            >
              Delete Permanently
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </DashboardLayout>
  );
}
