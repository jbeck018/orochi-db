import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { clusterApi } from "@/lib/api";
import type {
  Cluster,
  CreateClusterForm,
  UpdateClusterForm,
} from "@/types";

// Query keys
export const clusterKeys = {
  all: ["clusters"] as const,
  lists: () => [...clusterKeys.all, "list"] as const,
  list: (page: number, pageSize: number) =>
    [...clusterKeys.lists(), { page, pageSize }] as const,
  details: () => [...clusterKeys.all, "detail"] as const,
  detail: (id: string) => [...clusterKeys.details(), id] as const,
  metrics: (id: string) => [...clusterKeys.detail(id), "metrics"] as const,
  metricsHistory: (id: string, period: string) =>
    [...clusterKeys.detail(id), "metrics-history", period] as const,
  connectionString: (id: string) =>
    [...clusterKeys.detail(id), "connection-string"] as const,
};

// List clusters query
export function useClusters(page = 1, pageSize = 100) {
  return useQuery({
    queryKey: clusterKeys.list(page, pageSize),
    queryFn: () => clusterApi.list(page, pageSize),
    staleTime: 1000 * 30, // 30 seconds for cluster list
  });
}

// Single cluster query
export function useCluster(id: string) {
  return useQuery({
    queryKey: clusterKeys.detail(id),
    queryFn: () => clusterApi.get(id),
    enabled: !!id,
  });
}

// Cluster metrics query
export function useClusterMetrics(id: string) {
  return useQuery({
    queryKey: clusterKeys.metrics(id),
    queryFn: () => clusterApi.getMetrics(id),
    enabled: !!id,
    refetchInterval: 10000, // Refetch every 10 seconds
  });
}

// Cluster metrics history query
export function useClusterMetricsHistory(
  id: string,
  period: "1h" | "6h" | "24h" | "7d" | "30d" = "24h"
) {
  return useQuery({
    queryKey: clusterKeys.metricsHistory(id, period),
    queryFn: () => clusterApi.getMetricsHistory(id, period),
    enabled: !!id,
    staleTime: 1000 * 60 * 5, // 5 minutes
  });
}

// Connection string query
export function useConnectionString(id: string) {
  return useQuery({
    queryKey: clusterKeys.connectionString(id),
    queryFn: () => clusterApi.getConnectionString(id),
    enabled: !!id,
    staleTime: Infinity, // Connection string rarely changes
  });
}

// Create cluster mutation
export function useCreateCluster() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (data: CreateClusterForm) => clusterApi.create(data),
    onSuccess: () => {
      // Invalidate cluster list to refetch
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Update cluster mutation
export function useUpdateCluster(id: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (data: UpdateClusterForm) => clusterApi.update(id, data),
    onMutate: async (newData) => {
      // Cancel outgoing refetches
      await queryClient.cancelQueries({ queryKey: clusterKeys.detail(id) });

      // Snapshot previous value
      const previousCluster = queryClient.getQueryData(clusterKeys.detail(id));

      // Optimistically update
      queryClient.setQueryData(clusterKeys.detail(id), (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: { ...old.data, ...newData },
        };
      });

      return { previousCluster };
    },
    onError: (_err, _newData, context) => {
      // Rollback on error
      if (context?.previousCluster) {
        queryClient.setQueryData(clusterKeys.detail(id), context.previousCluster);
      }
    },
    onSettled: () => {
      // Refetch after error or success
      queryClient.invalidateQueries({ queryKey: clusterKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Delete cluster mutation
export function useDeleteCluster() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (id: string) => clusterApi.delete(id),
    onMutate: async (id) => {
      // Cancel outgoing refetches
      await queryClient.cancelQueries({ queryKey: clusterKeys.lists() });

      // Snapshot previous value
      const previousClusters = queryClient.getQueryData(clusterKeys.lists());

      // Optimistically remove from list
      queryClient.setQueriesData({ queryKey: clusterKeys.lists() }, (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: old.data.filter((cluster: Cluster) => cluster.id !== id),
        };
      });

      return { previousClusters };
    },
    onError: (_err, _id, context) => {
      // Rollback on error
      if (context?.previousClusters) {
        queryClient.setQueriesData(
          { queryKey: clusterKeys.lists() },
          context.previousClusters
        );
      }
    },
    onSettled: () => {
      // Refetch after error or success
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Start cluster mutation
export function useStartCluster(id: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => clusterApi.start(id),
    onMutate: async () => {
      await queryClient.cancelQueries({ queryKey: clusterKeys.detail(id) });
      const previousCluster = queryClient.getQueryData(clusterKeys.detail(id));

      // Optimistically update status
      queryClient.setQueryData(clusterKeys.detail(id), (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: { ...old.data, status: "creating" as const },
        };
      });

      return { previousCluster };
    },
    onError: (_err, _vars, context) => {
      if (context?.previousCluster) {
        queryClient.setQueryData(clusterKeys.detail(id), context.previousCluster);
      }
    },
    onSettled: () => {
      queryClient.invalidateQueries({ queryKey: clusterKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Stop cluster mutation
export function useStopCluster(id: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => clusterApi.stop(id),
    onMutate: async () => {
      await queryClient.cancelQueries({ queryKey: clusterKeys.detail(id) });
      const previousCluster = queryClient.getQueryData(clusterKeys.detail(id));

      // Optimistically update status
      queryClient.setQueryData(clusterKeys.detail(id), (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: { ...old.data, status: "stopped" as const },
        };
      });

      return { previousCluster };
    },
    onError: (_err, _vars, context) => {
      if (context?.previousCluster) {
        queryClient.setQueryData(clusterKeys.detail(id), context.previousCluster);
      }
    },
    onSettled: () => {
      queryClient.invalidateQueries({ queryKey: clusterKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Restart cluster mutation
export function useRestartCluster(id: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: () => clusterApi.restart(id),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: clusterKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}

// Reset password mutation
export function useResetClusterPassword(id: string) {
  return useMutation({
    mutationFn: () => clusterApi.resetPassword(id),
  });
}

// Scale cluster mutation (updates node count)
export function useScaleCluster(id: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (nodeCount: number) =>
      clusterApi.update(id, { nodeCount }),
    onMutate: async (nodeCount) => {
      await queryClient.cancelQueries({ queryKey: clusterKeys.detail(id) });
      const previousCluster = queryClient.getQueryData(clusterKeys.detail(id));

      // Optimistically update node count
      queryClient.setQueryData(clusterKeys.detail(id), (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: {
            ...old.data,
            config: { ...old.data.config, nodeCount },
          },
        };
      });

      return { previousCluster };
    },
    onError: (_err, _nodeCount, context) => {
      if (context?.previousCluster) {
        queryClient.setQueryData(clusterKeys.detail(id), context.previousCluster);
      }
    },
    onSettled: () => {
      queryClient.invalidateQueries({ queryKey: clusterKeys.detail(id) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}
