import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { poolerApi } from "@/lib/api";
import type {
  PoolerStatus,
  PoolerStats,
  PoolerConfig,
  PoolerStatsHistory,
  PoolerClientsResponse,
  PoolerPoolsResponse,
  UpdatePoolerConfigRequest,
} from "@/types/pooler";
import type { ApiResponse } from "@/types";

// Query keys for pooler
export const poolerKeys = {
  all: ["pooler"] as const,
  status: (clusterId: string) => [...poolerKeys.all, "status", clusterId] as const,
  stats: (clusterId: string) => [...poolerKeys.all, "stats", clusterId] as const,
  statsHistory: (clusterId: string, period: string) =>
    [...poolerKeys.all, "stats-history", clusterId, period] as const,
  config: (clusterId: string) => [...poolerKeys.all, "config", clusterId] as const,
  clients: (clusterId: string, page: number, pageSize: number) =>
    [...poolerKeys.all, "clients", clusterId, page, pageSize] as const,
  pools: (clusterId: string) => [...poolerKeys.all, "pools", clusterId] as const,
};

/**
 * Hook to fetch pooler status including health, config summary, and stats
 */
export function usePoolerStatus(clusterId: string) {
  return useQuery({
    queryKey: poolerKeys.status(clusterId),
    queryFn: () => poolerApi.getStatus(clusterId),
    enabled: !!clusterId,
    staleTime: 1000 * 10, // 10 seconds
  });
}

/**
 * Hook to fetch real-time pooler stats with configurable polling
 */
export function usePoolerStats(
  clusterId: string,
  options?: { refetchInterval?: number }
) {
  return useQuery({
    queryKey: poolerKeys.stats(clusterId),
    queryFn: () => poolerApi.getStats(clusterId),
    enabled: !!clusterId,
    refetchInterval: options?.refetchInterval ?? 5000, // Default 5 second polling
    staleTime: 1000 * 2, // 2 seconds
  });
}

/**
 * Hook to fetch pooler stats history for charts
 */
export function usePoolerStatsHistory(
  clusterId: string,
  period: "1h" | "6h" | "24h" | "7d" = "1h"
) {
  return useQuery({
    queryKey: poolerKeys.statsHistory(clusterId, period),
    queryFn: () => poolerApi.getStatsHistory(clusterId, period),
    enabled: !!clusterId,
    staleTime: 1000 * 60, // 1 minute
  });
}

/**
 * Hook to fetch pooler configuration
 */
export function usePoolerConfig(clusterId: string) {
  return useQuery({
    queryKey: poolerKeys.config(clusterId),
    queryFn: () => poolerApi.getConfig(clusterId),
    enabled: !!clusterId,
    staleTime: 1000 * 30, // 30 seconds
  });
}

/**
 * Hook to update pooler configuration
 */
export function useUpdatePoolerConfig(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (config: UpdatePoolerConfigRequest) =>
      poolerApi.updateConfig(clusterId, config),
    onMutate: async (newConfig) => {
      // Cancel outgoing refetches
      await queryClient.cancelQueries({ queryKey: poolerKeys.config(clusterId) });
      await queryClient.cancelQueries({ queryKey: poolerKeys.status(clusterId) });

      // Snapshot previous values
      const previousConfig = queryClient.getQueryData(poolerKeys.config(clusterId));
      const previousStatus = queryClient.getQueryData(poolerKeys.status(clusterId));

      // Optimistically update config
      queryClient.setQueryData(
        poolerKeys.config(clusterId),
        (old: ApiResponse<PoolerConfig> | undefined) => {
          if (!old?.data) return old;
          return {
            ...old,
            data: { ...old.data, ...newConfig },
          };
        }
      );

      return { previousConfig, previousStatus };
    },
    onError: (_err, _newConfig, context) => {
      // Rollback on error
      if (context?.previousConfig) {
        queryClient.setQueryData(poolerKeys.config(clusterId), context.previousConfig);
      }
      if (context?.previousStatus) {
        queryClient.setQueryData(poolerKeys.status(clusterId), context.previousStatus);
      }
    },
    onSettled: () => {
      // Refetch after mutation
      queryClient.invalidateQueries({ queryKey: poolerKeys.config(clusterId) });
      queryClient.invalidateQueries({ queryKey: poolerKeys.status(clusterId) });
    },
  });
}

/**
 * Hook to reload pooler configuration (hot reload without restart)
 */
export function useReloadPoolerConfig(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => poolerApi.reloadConfig(clusterId),
    onSuccess: () => {
      // Invalidate all pooler queries to refetch fresh data
      queryClient.invalidateQueries({ queryKey: poolerKeys.status(clusterId) });
      queryClient.invalidateQueries({ queryKey: poolerKeys.config(clusterId) });
      queryClient.invalidateQueries({ queryKey: poolerKeys.stats(clusterId) });
    },
  });
}

/**
 * Hook to fetch connected clients with pagination
 */
export function usePoolerClients(
  clusterId: string,
  page = 1,
  pageSize = 50
) {
  return useQuery({
    queryKey: poolerKeys.clients(clusterId, page, pageSize),
    queryFn: () => poolerApi.getClients(clusterId, page, pageSize),
    enabled: !!clusterId,
    staleTime: 1000 * 5, // 5 seconds
  });
}

/**
 * Hook to fetch connection pools
 */
export function usePoolerPools(clusterId: string) {
  return useQuery({
    queryKey: poolerKeys.pools(clusterId),
    queryFn: () => poolerApi.getPools(clusterId),
    enabled: !!clusterId,
    staleTime: 1000 * 5, // 5 seconds
  });
}

/**
 * Hook to disconnect a client
 */
export function useDisconnectPoolerClient(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (clientId: string) => poolerApi.disconnectClient(clusterId, clientId),
    onMutate: async (clientId) => {
      // Cancel outgoing refetches
      await queryClient.cancelQueries({
        queryKey: poolerKeys.clients(clusterId, 1, 50),
      });

      // Optimistically remove client from list
      queryClient.setQueriesData(
        { queryKey: [...poolerKeys.all, "clients", clusterId] },
        (old: PoolerClientsResponse | undefined) => {
          if (!old?.clients) return old;
          return {
            ...old,
            clients: old.clients.filter((c) => c.id !== clientId),
            totalCount: old.totalCount - 1,
          };
        }
      );
    },
    onSettled: () => {
      // Refetch clients and stats
      queryClient.invalidateQueries({
        queryKey: [...poolerKeys.all, "clients", clusterId],
      });
      queryClient.invalidateQueries({ queryKey: poolerKeys.stats(clusterId) });
    },
  });
}

/**
 * Hook to pause the pooler (stop accepting new connections)
 */
export function usePausePooler(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => poolerApi.pause(clusterId),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: poolerKeys.status(clusterId) });
    },
  });
}

/**
 * Hook to resume the pooler
 */
export function useResumePooler(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => poolerApi.resume(clusterId),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: poolerKeys.status(clusterId) });
    },
  });
}
