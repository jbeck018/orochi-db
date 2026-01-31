"use client";

import * as React from "react";
import {
  RefreshCw,
  XCircle,
  ChevronLeft,
  ChevronRight,
  ChevronsLeft,
  ChevronsRight,
  Search,
  Filter,
  Clock,
  Database,
  User,
  Activity,
} from "lucide-react";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Badge } from "@/components/ui/badge";
import { Skeleton } from "@/components/ui/skeleton";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from "@/components/ui/alert-dialog";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { useToast } from "@/hooks/use-toast";
import { usePoolerClients, useDisconnectPoolerClient } from "@/hooks/api";
import { formatRelativeTime } from "@/lib/utils";
import type { PoolerClient } from "@/types/pooler";

interface PoolerClientsTableProps {
  clusterId: string;
}

type ClientState = PoolerClient["state"];

const stateColors: Record<ClientState, string> = {
  active: "bg-green-100 text-green-700 dark:bg-green-900 dark:text-green-300",
  idle: "bg-blue-100 text-blue-700 dark:bg-blue-900 dark:text-blue-300",
  waiting: "bg-yellow-100 text-yellow-700 dark:bg-yellow-900 dark:text-yellow-300",
  transaction: "bg-purple-100 text-purple-700 dark:bg-purple-900 dark:text-purple-300",
};

export const PoolerClientsTable = React.memo(function PoolerClientsTable({
  clusterId,
}: PoolerClientsTableProps): React.JSX.Element {
  const { toast } = useToast();

  const [page, setPage] = React.useState(1);
  const [pageSize, setPageSize] = React.useState(20);
  const [search, setSearch] = React.useState("");
  const [stateFilter, setStateFilter] = React.useState<ClientState | "all">("all");

  const { data: clientsData, isLoading, refetch } = usePoolerClients(
    clusterId,
    page,
    pageSize
  );
  const disconnectMutation = useDisconnectPoolerClient(clusterId);

  const clients = clientsData?.clients ?? [];
  const totalCount = clientsData?.totalCount ?? 0;
  const totalPages = Math.ceil(totalCount / pageSize);

  // Filter clients locally for search and state filter
  const filteredClients = React.useMemo(() => {
    return clients.filter((client) => {
      // State filter
      if (stateFilter !== "all" && client.state !== stateFilter) {
        return false;
      }

      // Search filter
      if (search) {
        const searchLower = search.toLowerCase();
        return (
          client.database.toLowerCase().includes(searchLower) ||
          client.user.toLowerCase().includes(searchLower) ||
          client.clientAddress.toLowerCase().includes(searchLower) ||
          (client.applicationName?.toLowerCase().includes(searchLower) ?? false)
        );
      }

      return true;
    });
  }, [clients, search, stateFilter]);

  const handleDisconnect = async (clientId: string): Promise<void> => {
    try {
      await disconnectMutation.mutateAsync(clientId);
      toast({
        title: "Client disconnected",
        description: "The client connection has been terminated",
      });
    } catch (err) {
      toast({
        title: "Disconnect failed",
        description: err instanceof Error ? err.message : "Failed to disconnect client",
        variant: "destructive",
      });
    }
  };

  const formatDuration = (ms: number | undefined): string => {
    if (ms === undefined) return "-";
    if (ms < 1000) return `${ms.toFixed(0)}ms`;
    if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`;
    return `${(ms / 60000).toFixed(1)}m`;
  };

  if (isLoading) {
    return (
      <Card>
        <CardHeader>
          <Skeleton className="h-6 w-40" />
          <Skeleton className="h-4 w-64" />
        </CardHeader>
        <CardContent>
          <div className="space-y-4">
            <div className="flex gap-4">
              <Skeleton className="h-10 w-64" />
              <Skeleton className="h-10 w-32" />
            </div>
            <Skeleton className="h-[400px] w-full" />
          </div>
        </CardContent>
      </Card>
    );
  }

  return (
    <TooltipProvider>
      <Card>
        <CardHeader>
          <div className="flex items-center justify-between">
            <div>
              <CardTitle className="flex items-center gap-2">
                <Activity className="h-5 w-5" />
                Connected Clients
              </CardTitle>
              <CardDescription>
                {totalCount} total client{totalCount !== 1 ? "s" : ""} connected to the pooler
              </CardDescription>
            </div>
            <Button
              variant="outline"
              size="sm"
              onClick={() => refetch()}
            >
              <RefreshCw className="h-4 w-4" />
            </Button>
          </div>
        </CardHeader>
        <CardContent className="space-y-4">
          {/* Filters */}
          <div className="flex flex-col gap-4 sm:flex-row sm:items-center">
            <div className="relative flex-1">
              <Search className="absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted-foreground" />
              <Input
                placeholder="Search by database, user, or address..."
                value={search}
                onChange={(e) => setSearch(e.target.value)}
                className="pl-10"
              />
            </div>
            <div className="flex gap-2">
              <Select
                value={stateFilter}
                onValueChange={(value) => setStateFilter(value as ClientState | "all")}
              >
                <SelectTrigger className="w-[140px]">
                  <Filter className="mr-2 h-4 w-4" />
                  <SelectValue placeholder="State" />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="all">All States</SelectItem>
                  <SelectItem value="active">Active</SelectItem>
                  <SelectItem value="idle">Idle</SelectItem>
                  <SelectItem value="waiting">Waiting</SelectItem>
                  <SelectItem value="transaction">In Transaction</SelectItem>
                </SelectContent>
              </Select>
              <Select
                value={pageSize.toString()}
                onValueChange={(value) => {
                  setPageSize(parseInt(value));
                  setPage(1);
                }}
              >
                <SelectTrigger className="w-[100px]">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="10">10 / page</SelectItem>
                  <SelectItem value="20">20 / page</SelectItem>
                  <SelectItem value="50">50 / page</SelectItem>
                  <SelectItem value="100">100 / page</SelectItem>
                </SelectContent>
              </Select>
            </div>
          </div>

          {/* Table */}
          <div className="rounded-md border">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>Database</TableHead>
                  <TableHead>User</TableHead>
                  <TableHead>State</TableHead>
                  <TableHead>Client Address</TableHead>
                  <TableHead>Connected</TableHead>
                  <TableHead>Duration</TableHead>
                  <TableHead className="w-[80px]">Actions</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {filteredClients.length === 0 ? (
                  <TableRow>
                    <TableCell colSpan={7} className="h-24 text-center">
                      <p className="text-muted-foreground">
                        {search || stateFilter !== "all"
                          ? "No clients match your filters"
                          : "No clients currently connected"}
                      </p>
                    </TableCell>
                  </TableRow>
                ) : (
                  filteredClients.map((client) => (
                    <TableRow key={client.id}>
                      <TableCell>
                        <div className="flex items-center gap-2">
                          <Database className="h-4 w-4 text-muted-foreground" />
                          <span className="font-mono text-sm">{client.database}</span>
                        </div>
                      </TableCell>
                      <TableCell>
                        <div className="flex items-center gap-2">
                          <User className="h-4 w-4 text-muted-foreground" />
                          <div>
                            <span className="font-mono text-sm">{client.user}</span>
                            {client.applicationName && (
                              <p className="text-xs text-muted-foreground">
                                {client.applicationName}
                              </p>
                            )}
                          </div>
                        </div>
                      </TableCell>
                      <TableCell>
                        <Badge variant="outline" className={stateColors[client.state]}>
                          {client.state}
                        </Badge>
                      </TableCell>
                      <TableCell>
                        <span className="font-mono text-sm">{client.clientAddress}</span>
                      </TableCell>
                      <TableCell>
                        <Tooltip>
                          <TooltipTrigger>
                            <span className="text-sm">
                              {formatRelativeTime(client.connectTime)}
                            </span>
                          </TooltipTrigger>
                          <TooltipContent>
                            {new Date(client.connectTime).toLocaleString()}
                          </TooltipContent>
                        </Tooltip>
                      </TableCell>
                      <TableCell>
                        <div className="flex items-center gap-1 text-sm">
                          <Clock className="h-3 w-3 text-muted-foreground" />
                          {client.state === "active" && client.queryDurationMs !== undefined
                            ? formatDuration(client.queryDurationMs)
                            : client.state === "transaction" && client.transactionDurationMs !== undefined
                            ? formatDuration(client.transactionDurationMs)
                            : client.state === "waiting" && client.waitDurationMs !== undefined
                            ? formatDuration(client.waitDurationMs)
                            : "-"}
                        </div>
                      </TableCell>
                      <TableCell>
                        <AlertDialog>
                          <AlertDialogTrigger asChild>
                            <Button
                              variant="ghost"
                              size="sm"
                              className="h-8 w-8 p-0 text-red-500 hover:text-red-600"
                            >
                              <XCircle className="h-4 w-4" />
                            </Button>
                          </AlertDialogTrigger>
                          <AlertDialogContent>
                            <AlertDialogHeader>
                              <AlertDialogTitle>Disconnect Client</AlertDialogTitle>
                              <AlertDialogDescription>
                                Are you sure you want to disconnect this client? This will
                                terminate their connection immediately.
                                <div className="mt-2 rounded-md bg-muted p-2 text-sm">
                                  <p>
                                    <strong>Database:</strong> {client.database}
                                  </p>
                                  <p>
                                    <strong>User:</strong> {client.user}
                                  </p>
                                  <p>
                                    <strong>Address:</strong> {client.clientAddress}
                                  </p>
                                </div>
                              </AlertDialogDescription>
                            </AlertDialogHeader>
                            <AlertDialogFooter>
                              <AlertDialogCancel>Cancel</AlertDialogCancel>
                              <AlertDialogAction
                                onClick={() => handleDisconnect(client.id)}
                                className="bg-red-500 hover:bg-red-600"
                              >
                                Disconnect
                              </AlertDialogAction>
                            </AlertDialogFooter>
                          </AlertDialogContent>
                        </AlertDialog>
                      </TableCell>
                    </TableRow>
                  ))
                )}
              </TableBody>
            </Table>
          </div>

          {/* Pagination */}
          {totalPages > 1 && (
            <div className="flex items-center justify-between">
              <p className="text-sm text-muted-foreground">
                Showing {(page - 1) * pageSize + 1} to{" "}
                {Math.min(page * pageSize, totalCount)} of {totalCount} clients
              </p>
              <div className="flex items-center gap-2">
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => setPage(1)}
                  disabled={page === 1}
                >
                  <ChevronsLeft className="h-4 w-4" />
                </Button>
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => setPage((p) => Math.max(1, p - 1))}
                  disabled={page === 1}
                >
                  <ChevronLeft className="h-4 w-4" />
                </Button>
                <span className="text-sm">
                  Page {page} of {totalPages}
                </span>
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => setPage((p) => Math.min(totalPages, p + 1))}
                  disabled={page === totalPages}
                >
                  <ChevronRight className="h-4 w-4" />
                </Button>
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => setPage(totalPages)}
                  disabled={page === totalPages}
                >
                  <ChevronsRight className="h-4 w-4" />
                </Button>
              </div>
            </div>
          )}
        </CardContent>
      </Card>
    </TooltipProvider>
  );
});
