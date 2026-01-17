import { useMutation, useQueryClient } from "@tanstack/react-query";
import { useNavigate } from "@tanstack/react-router";
import {
  login as loginFn,
  logout as logoutFn,
  register as registerFn,
} from "@/lib/auth";
import { passwordResetApi } from "@/lib/api";
import type { LoginCredentials, RegisterCredentials } from "@/types";
import { userKeys } from "./useUser";

// Login mutation
export function useLogin() {
  const queryClient = useQueryClient();
  const navigate = useNavigate();

  return useMutation({
    mutationFn: (credentials: LoginCredentials) => loginFn(credentials),
    onSuccess: (data) => {
      // Set user data in cache
      queryClient.setQueryData(userKeys.me(), { data: data.user });
      // Navigate to clusters page
      navigate({ to: "/clusters" });
    },
  });
}

// Register mutation
export function useRegister() {
  const queryClient = useQueryClient();
  const navigate = useNavigate();

  return useMutation({
    mutationFn: (credentials: RegisterCredentials) => registerFn(credentials),
    onSuccess: (data) => {
      // Set user data in cache
      queryClient.setQueryData(userKeys.me(), { data: data.user });
      // Navigate to clusters page
      navigate({ to: "/clusters" });
    },
  });
}

// Logout mutation
export function useLogout() {
  const queryClient = useQueryClient();
  const navigate = useNavigate();

  return useMutation({
    mutationFn: () => logoutFn(),
    onSuccess: () => {
      // Clear all cached data
      queryClient.clear();
      // Navigate to login
      navigate({ to: "/login" });
    },
  });
}

// Forgot password mutation
export function useForgotPassword() {
  return useMutation({
    mutationFn: (email: string) => passwordResetApi.requestReset(email),
  });
}

// Reset password mutation
export function useResetPassword() {
  const navigate = useNavigate();

  return useMutation({
    mutationFn: ({
      token,
      newPassword,
    }: {
      token: string;
      newPassword: string;
    }) => passwordResetApi.resetPassword(token, newPassword),
    onSuccess: () => {
      // Navigate to login after successful password reset
      navigate({ to: "/login" });
    },
  });
}
