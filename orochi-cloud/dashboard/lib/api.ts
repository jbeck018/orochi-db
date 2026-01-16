"use client";

import { getAuthHeader, refreshTokens, clearAuth } from "./auth";
import type {
  Cluster,
  ClusterMetrics,
  ClusterMetricsHistory,
  CreateClusterForm,
  UpdateClusterForm,
  UpdateUserForm,
  User,
  ApiResponse,
  PaginatedResponse,
} from "@/types";

const API_URL = process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8080";

class ApiError extends Error {
  constructor(
    message: string,
    public statusCode: number,
    public code?: string
  ) {
    super(message);
    this.name = "ApiError";
  }
}

async function fetchWithAuth<T>(
  endpoint: string,
  options: RequestInit = {}
): Promise<T> {
  const url = `${API_URL}${endpoint}`;
  const headers = {
    "Content-Type": "application/json",
    ...getAuthHeader(),
    ...options.headers,
  };

  let response = await fetch(url, { ...options, headers });

  // Handle token refresh on 401
  if (response.status === 401) {
    const newTokens = await refreshTokens();
    if (newTokens) {
      headers.Authorization = `Bearer ${newTokens.accessToken}`;
      response = await fetch(url, { ...options, headers });
    } else {
      clearAuth();
      throw new ApiError("Unauthorized", 401, "UNAUTHORIZED");
    }
  }

  if (!response.ok) {
    const errorData = await response.json().catch(() => ({}));
    throw new ApiError(
      errorData.error ?? `Request failed with status ${response.status}`,
      response.status,
      errorData.code
    );
  }

  // Handle empty responses
  const text = await response.text();
  if (!text) {
    return {} as T;
  }

  return JSON.parse(text) as T;
}

// Cluster API
export const clusterApi = {
  list: async (page = 1, pageSize = 10): Promise<PaginatedResponse<Cluster>> => {
    return fetchWithAuth<PaginatedResponse<Cluster>>(
      `/api/v1/clusters?page=${page}&pageSize=${pageSize}`
    );
  },

  get: async (id: string): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>(`/api/v1/clusters/${id}`);
  },

  create: async (data: CreateClusterForm): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>("/api/v1/clusters", {
      method: "POST",
      body: JSON.stringify(data),
    });
  },

  update: async (id: string, data: UpdateClusterForm): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>(`/api/v1/clusters/${id}`, {
      method: "PATCH",
      body: JSON.stringify(data),
    });
  },

  delete: async (id: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/clusters/${id}`, {
      method: "DELETE",
    });
  },

  start: async (id: string): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>(`/api/v1/clusters/${id}/start`, {
      method: "POST",
    });
  },

  stop: async (id: string): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>(`/api/v1/clusters/${id}/stop`, {
      method: "POST",
    });
  },

  restart: async (id: string): Promise<ApiResponse<Cluster>> => {
    return fetchWithAuth<ApiResponse<Cluster>>(`/api/v1/clusters/${id}/restart`, {
      method: "POST",
    });
  },

  getMetrics: async (id: string): Promise<ApiResponse<ClusterMetrics>> => {
    return fetchWithAuth<ApiResponse<ClusterMetrics>>(`/api/v1/clusters/${id}/metrics`);
  },

  getMetricsHistory: async (
    id: string,
    period: "1h" | "6h" | "24h" | "7d" | "30d" = "24h"
  ): Promise<ApiResponse<ClusterMetricsHistory>> => {
    return fetchWithAuth<ApiResponse<ClusterMetricsHistory>>(
      `/api/v1/clusters/${id}/metrics/history?period=${period}`
    );
  },

  getConnectionString: async (id: string): Promise<ApiResponse<{ connectionString: string }>> => {
    return fetchWithAuth<ApiResponse<{ connectionString: string }>>(
      `/api/v1/clusters/${id}/connection-string`
    );
  },

  resetPassword: async (id: string): Promise<ApiResponse<{ password: string }>> => {
    return fetchWithAuth<ApiResponse<{ password: string }>>(
      `/api/v1/clusters/${id}/reset-password`,
      { method: "POST" }
    );
  },
};

// User API
export const userApi = {
  getMe: async (): Promise<ApiResponse<User>> => {
    return fetchWithAuth<ApiResponse<User>>("/api/v1/auth/me");
  },

  update: async (data: UpdateUserForm): Promise<ApiResponse<User>> => {
    return fetchWithAuth<ApiResponse<User>>("/api/v1/users/me", {
      method: "PATCH",
      body: JSON.stringify(data),
    });
  },

  changePassword: async (currentPassword: string, newPassword: string): Promise<void> => {
    await fetchWithAuth<void>("/api/v1/users/me/password", {
      method: "POST",
      body: JSON.stringify({ currentPassword, newPassword }),
    });
  },

  deleteAccount: async (password: string): Promise<void> => {
    await fetchWithAuth<void>("/api/v1/users/me", {
      method: "DELETE",
      body: JSON.stringify({ password }),
    });
  },
};

// Export error class
export { ApiError };
