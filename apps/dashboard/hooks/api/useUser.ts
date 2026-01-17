import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { userApi, configApi, notificationApi, twoFactorApi } from "@/lib/api";
import type { UpdateUserForm } from "@/types";

// Query keys
export const userKeys = {
  all: ["user"] as const,
  me: () => [...userKeys.all, "me"] as const,
  notifications: () => [...userKeys.all, "notifications"] as const,
  twoFactor: () => [...userKeys.all, "2fa"] as const,
};

export const configKeys = {
  all: ["config"] as const,
  providers: () => [...configKeys.all, "providers"] as const,
  tiers: () => [...configKeys.all, "tiers"] as const,
  health: () => [...configKeys.all, "health"] as const,
};

// Get current user
export function useUser() {
  return useQuery({
    queryKey: userKeys.me(),
    queryFn: () => userApi.getMe(),
    staleTime: 1000 * 60 * 5, // 5 minutes
  });
}

// Update user mutation
export function useUpdateUser() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (data: UpdateUserForm) => userApi.update(data),
    onMutate: async (newData) => {
      await queryClient.cancelQueries({ queryKey: userKeys.me() });
      const previousUser = queryClient.getQueryData(userKeys.me());

      // Optimistically update
      queryClient.setQueryData(userKeys.me(), (old: any) => {
        if (!old?.data) return old;
        return {
          ...old,
          data: { ...old.data, ...newData },
        };
      });

      return { previousUser };
    },
    onError: (_err, _newData, context) => {
      if (context?.previousUser) {
        queryClient.setQueryData(userKeys.me(), context.previousUser);
      }
    },
    onSettled: () => {
      queryClient.invalidateQueries({ queryKey: userKeys.me() });
    },
  });
}

// Change password mutation
export function useChangePassword() {
  return useMutation({
    mutationFn: ({
      currentPassword,
      newPassword,
    }: {
      currentPassword: string;
      newPassword: string;
    }) => userApi.changePassword(currentPassword, newPassword),
  });
}

// Delete account mutation
export function useDeleteAccount() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (password: string) => userApi.deleteAccount(password),
    onSuccess: () => {
      // Clear all queries on account deletion
      queryClient.clear();
    },
  });
}

// Get providers
export function useProviders() {
  return useQuery({
    queryKey: configKeys.providers(),
    queryFn: () => configApi.getProviders(),
    staleTime: 1000 * 60 * 60, // 1 hour - providers rarely change
  });
}

// Get tiers
export function useTiers() {
  return useQuery({
    queryKey: configKeys.tiers(),
    queryFn: () => configApi.getTiers(),
    staleTime: 1000 * 60 * 60, // 1 hour
  });
}

// Get system health
export function useSystemHealth() {
  return useQuery({
    queryKey: configKeys.health(),
    queryFn: () => configApi.getSystemHealth(),
    refetchInterval: 1000 * 60, // Refetch every minute
  });
}

// Get notification preferences
export function useNotificationPreferences() {
  return useQuery({
    queryKey: userKeys.notifications(),
    queryFn: () => notificationApi.get(),
  });
}

// Update notification preferences
export function useUpdateNotificationPreferences() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: notificationApi.update,
    onMutate: async (newPreferences) => {
      await queryClient.cancelQueries({ queryKey: userKeys.notifications() });
      const previousPreferences = queryClient.getQueryData(
        userKeys.notifications()
      );

      // Optimistically update
      queryClient.setQueryData(userKeys.notifications(), {
        data: newPreferences,
      });

      return { previousPreferences };
    },
    onError: (_err, _newData, context) => {
      if (context?.previousPreferences) {
        queryClient.setQueryData(
          userKeys.notifications(),
          context.previousPreferences
        );
      }
    },
    onSettled: () => {
      queryClient.invalidateQueries({ queryKey: userKeys.notifications() });
    },
  });
}

// Get 2FA status
export function useTwoFactorStatus() {
  return useQuery({
    queryKey: userKeys.twoFactor(),
    queryFn: () => twoFactorApi.getStatus(),
  });
}

// Enable 2FA
export function useEnableTwoFactor() {
  return useMutation({
    mutationFn: () => twoFactorApi.enable(),
  });
}

// Verify 2FA setup
export function useVerifyTwoFactor() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (code: string) => twoFactorApi.verify(code),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: userKeys.twoFactor() });
    },
  });
}

// Disable 2FA
export function useDisableTwoFactor() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: (code: string) => twoFactorApi.disable(code),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: userKeys.twoFactor() });
    },
  });
}
