import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { branchApi } from "@/lib/api";
import type { Branch, CreateBranchForm } from "@/types";
import { clusterKeys } from "./useClusters";

// Query keys
export const branchKeys = {
  all: ["branches"] as const,
  lists: () => [...branchKeys.all, "list"] as const,
  list: (clusterId: string) => [...branchKeys.lists(), clusterId] as const,
  details: () => [...branchKeys.all, "detail"] as const,
  detail: (clusterId: string, branchId: string) =>
    [...branchKeys.details(), clusterId, branchId] as const,
};

// List branches for a cluster
export function useBranches(clusterId: string) {
  return useQuery({
    queryKey: branchKeys.list(clusterId),
    queryFn: () => branchApi.list(clusterId),
    enabled: !!clusterId,
    staleTime: 1000 * 10, // 10 seconds
  });
}

// Single branch query
export function useBranch(clusterId: string, branchId: string) {
  return useQuery({
    queryKey: branchKeys.detail(clusterId, branchId),
    queryFn: () => branchApi.get(clusterId, branchId),
    enabled: !!clusterId && !!branchId,
    refetchInterval: (query) => {
      // Poll more frequently while branch is creating
      const branch = query.state.data as Branch | undefined;
      if (branch?.status === "creating") {
        return 3000; // 3 seconds
      }
      return false;
    },
  });
}

// Create branch mutation
export function useCreateBranch(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (data: CreateBranchForm) => branchApi.create(clusterId, data),
    onSuccess: () => {
      // Invalidate branch list to refetch
      queryClient.invalidateQueries({ queryKey: branchKeys.list(clusterId) });
    },
  });
}

// Delete branch mutation
export function useDeleteBranch(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (branchId: string) => branchApi.delete(clusterId, branchId),
    onMutate: async (branchId) => {
      // Cancel outgoing refetches
      await queryClient.cancelQueries({ queryKey: branchKeys.list(clusterId) });

      // Snapshot previous value
      const previousBranches = queryClient.getQueryData(
        branchKeys.list(clusterId)
      );

      // Optimistically remove from list
      queryClient.setQueryData(
        branchKeys.list(clusterId),
        (old: { branches: Branch[]; totalCount: number } | undefined) => {
          if (!old) return old;
          return {
            branches: old.branches.filter((branch) => branch.id !== branchId),
            totalCount: old.totalCount - 1,
          };
        }
      );

      return { previousBranches };
    },
    onError: (_err, _branchId, context) => {
      // Rollback on error
      if (context?.previousBranches) {
        queryClient.setQueryData(
          branchKeys.list(clusterId),
          context.previousBranches
        );
      }
    },
    onSettled: () => {
      // Refetch after error or success
      queryClient.invalidateQueries({ queryKey: branchKeys.list(clusterId) });
    },
  });
}

// Promote branch mutation (promote to standalone cluster)
export function usePromoteBranch(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (branchId: string) => branchApi.promote(clusterId, branchId),
    onMutate: async (branchId) => {
      await queryClient.cancelQueries({
        queryKey: branchKeys.detail(clusterId, branchId),
      });

      const previousBranch = queryClient.getQueryData(
        branchKeys.detail(clusterId, branchId)
      );

      // Optimistically update status to promoting
      queryClient.setQueryData(
        branchKeys.detail(clusterId, branchId),
        (old: Branch | undefined) => {
          if (!old) return old;
          return { ...old, status: "promoting" as const };
        }
      );

      return { previousBranch };
    },
    onError: (_err, branchId, context) => {
      if (context?.previousBranch) {
        queryClient.setQueryData(
          branchKeys.detail(clusterId, branchId),
          context.previousBranch
        );
      }
    },
    onSettled: () => {
      // Refetch branches and clusters
      queryClient.invalidateQueries({ queryKey: branchKeys.list(clusterId) });
      queryClient.invalidateQueries({ queryKey: clusterKeys.lists() });
    },
  });
}
