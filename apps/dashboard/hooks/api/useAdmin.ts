import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { adminApi } from "@/lib/api";
import type { UserRole } from "@/types";

// Query keys
export const adminKeys = {
  all: ["admin"] as const,
  stats: () => [...adminKeys.all, "stats"] as const,
  users: () => [...adminKeys.all, "users"] as const,
  usersList: (page: number, pageSize: number, search?: string) =>
    [...adminKeys.users(), "list", { page, pageSize, search }] as const,
  user: (id: string) => [...adminKeys.users(), "detail", id] as const,
  clusters: () => [...adminKeys.all, "clusters"] as const,
  clustersList: (page: number, pageSize: number, status?: string) =>
    [...adminKeys.clusters(), "list", { page, pageSize, status }] as const,
  cluster: (id: string) => [...adminKeys.clusters(), "detail", id] as const,
};

// Admin stats query
export function useAdminStats() {
  return useQuery({
    queryKey: adminKeys.stats(),
    queryFn: () => adminApi.getStats(),
    staleTime: 1000 * 60, // 1 minute
  });
}

// Admin users list query
export function useAdminUsers(page = 1, pageSize = 20, search?: string) {
  return useQuery({
    queryKey: adminKeys.usersList(page, pageSize, search),
    queryFn: () => adminApi.listUsers(page, pageSize, search),
    staleTime: 1000 * 30, // 30 seconds
  });
}

// Single admin user query
export function useAdminUser(id: string) {
  return useQuery({
    queryKey: adminKeys.user(id),
    queryFn: () => adminApi.getUser(id),
    enabled: !!id,
  });
}

// Update user role mutation
export function useUpdateUserRole() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ id, role }: { id: string; role: UserRole }) =>
      adminApi.updateUserRole(id, role),
    onSuccess: (_data, { id }) => {
      queryClient.invalidateQueries({ queryKey: adminKeys.user(id) });
      queryClient.invalidateQueries({ queryKey: adminKeys.users() });
      queryClient.invalidateQueries({ queryKey: adminKeys.stats() });
    },
  });
}

// Set user active mutation
export function useSetUserActive() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ id, active }: { id: string; active: boolean }) =>
      adminApi.setUserActive(id, active),
    onSuccess: (_data, { id }) => {
      queryClient.invalidateQueries({ queryKey: adminKeys.user(id) });
      queryClient.invalidateQueries({ queryKey: adminKeys.users() });
      queryClient.invalidateQueries({ queryKey: adminKeys.stats() });
    },
  });
}

// Admin clusters list query
export function useAdminClusters(page = 1, pageSize = 20, status?: string) {
  return useQuery({
    queryKey: adminKeys.clustersList(page, pageSize, status),
    queryFn: () => adminApi.listClusters(page, pageSize, status),
    staleTime: 1000 * 30, // 30 seconds
  });
}

// Single admin cluster query
export function useAdminCluster(id: string) {
  return useQuery({
    queryKey: adminKeys.cluster(id),
    queryFn: () => adminApi.getCluster(id),
    enabled: !!id,
  });
}

// Force delete cluster mutation
export function useForceDeleteCluster() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (id: string) => adminApi.forceDeleteCluster(id),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: adminKeys.clusters() });
      queryClient.invalidateQueries({ queryKey: adminKeys.stats() });
    },
  });
}
