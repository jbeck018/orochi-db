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

const API_URL = import.meta.env.VITE_API_URL ?? "http://localhost:8080";

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
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
    ...getAuthHeader(),
    ...(options.headers as Record<string, string>),
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

// Providers and Tiers API
export const configApi = {
  getProviders: async (): Promise<ApiResponse<ProviderOption[]>> => {
    return fetchWithAuth<ApiResponse<ProviderOption[]>>("/api/v1/config/providers");
  },

  getTiers: async (): Promise<ApiResponse<TierOption[]>> => {
    return fetchWithAuth<ApiResponse<TierOption[]>>("/api/v1/config/tiers");
  },

  getSystemHealth: async (): Promise<ApiResponse<SystemHealth>> => {
    return fetchWithAuth<ApiResponse<SystemHealth>>("/api/v1/config/health");
  },
};

// Notification Preferences API
export const notificationApi = {
  get: async (): Promise<ApiResponse<NotificationPreferences>> => {
    return fetchWithAuth<ApiResponse<NotificationPreferences>>("/api/v1/users/me/notifications");
  },

  update: async (preferences: NotificationPreferences): Promise<ApiResponse<NotificationPreferences>> => {
    return fetchWithAuth<ApiResponse<NotificationPreferences>>("/api/v1/users/me/notifications", {
      method: "PUT",
      body: JSON.stringify(preferences),
    });
  },
};

// Two-Factor Authentication API
export const twoFactorApi = {
  getStatus: async (): Promise<ApiResponse<TwoFactorStatus>> => {
    return fetchWithAuth<ApiResponse<TwoFactorStatus>>("/api/v1/users/me/2fa");
  },

  enable: async (): Promise<ApiResponse<TwoFactorSetup>> => {
    return fetchWithAuth<ApiResponse<TwoFactorSetup>>("/api/v1/users/me/2fa/enable", {
      method: "POST",
    });
  },

  verify: async (code: string): Promise<ApiResponse<{ backupCodes: string[] }>> => {
    return fetchWithAuth<ApiResponse<{ backupCodes: string[] }>>("/api/v1/users/me/2fa/verify", {
      method: "POST",
      body: JSON.stringify({ code }),
    });
  },

  disable: async (code: string): Promise<void> => {
    await fetchWithAuth<void>("/api/v1/users/me/2fa/disable", {
      method: "POST",
      body: JSON.stringify({ code }),
    });
  },
};

// Password Reset API
export const passwordResetApi = {
  requestReset: async (email: string): Promise<void> => {
    await fetch(`${API_URL}/api/v1/auth/forgot-password`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ email }),
    });
  },

  resetPassword: async (token: string, newPassword: string): Promise<void> => {
    const response = await fetch(`${API_URL}/api/v1/auth/reset-password`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token, newPassword }),
    });

    if (!response.ok) {
      const error = await response.json().catch(() => ({ error: "Password reset failed" }));
      throw new ApiError(error.error ?? "Password reset failed", response.status);
    }
  },
};

// Types for new APIs
export interface ProviderOption {
  id: string;
  name: string;
  regions: RegionOption[];
}

export interface RegionOption {
  id: string;
  name: string;
  location: string;
}

export interface TierOption {
  id: string;
  name: string;
  description: string;
  price: string;
  features: string[];
  limits: {
    vcpu: number;
    memoryGb: number;
    storageGb: number;
  };
}

export interface SystemHealth {
  status: "operational" | "degraded" | "outage";
  services: {
    name: string;
    status: "operational" | "degraded" | "outage";
  }[];
  lastUpdated: string;
}

export interface NotificationPreferences {
  emailNotifications: boolean;
  alertNotifications: boolean;
  marketingEmails: boolean;
}

export interface TwoFactorStatus {
  enabled: boolean;
  enabledAt?: string;
}

export interface TwoFactorSetup {
  secret: string;
  qrCodeUrl: string;
  manualEntryKey: string;
}

// Export error class
export { ApiError };
