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
  ShardDistribution,
  TimeSeriesMetrics,
  ColumnarStats,
  ClusterTopology,
  PipelineMetrics,
  CDCMetrics,
} from "@/types";

const API_URL = import.meta.env.VITE_API_URL ?? "http://localhost:8080";

// Token refresh mutex to prevent concurrent refresh requests
let refreshPromise: Promise<Awaited<ReturnType<typeof refreshTokens>>> | null = null;

async function refreshTokensWithLock(): Promise<Awaited<ReturnType<typeof refreshTokens>>> {
  // If a refresh is already in progress, wait for it
  if (refreshPromise) {
    return refreshPromise;
  }

  // Start a new refresh and store the promise
  refreshPromise = refreshTokens().finally(() => {
    // Clear the promise after completion (success or failure)
    refreshPromise = null;
  });

  return refreshPromise;
}

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

  // Handle token refresh on 401 with mutex to prevent concurrent refreshes
  if (response.status === 401) {
    const newTokens = await refreshTokensWithLock();
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

// API Cluster type (snake_case from backend)
interface ApiCluster {
  id: string;
  name: string;
  owner_id: string;
  status: string;
  tier: string;
  provider: string;
  region: string;
  version: string;
  node_count: number;
  node_size: string;
  storage_gb: number;
  connection_url: string;
  maintenance_day: string;
  maintenance_hour: number;
  backup_enabled: boolean;
  backup_retention_days: number;
  created_at: string;
  updated_at: string;
}

// Transform API cluster to frontend Cluster type
function transformCluster(c: ApiCluster): Cluster {
  return {
    id: c.id,
    name: c.name,
    status: c.status as Cluster["status"],
    config: {
      tier: c.tier as Cluster["config"]["tier"],
      provider: c.provider as Cluster["config"]["provider"],
      region: c.region,
      nodeCount: c.node_count,
      storageGb: c.storage_gb,
      highAvailability: false,
      backupEnabled: c.backup_enabled,
      backupRetentionDays: c.backup_retention_days,
      maintenanceWindow: {
        dayOfWeek: ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"].indexOf(c.maintenance_day),
        hourUtc: c.maintenance_hour,
      },
    },
    connectionString: c.connection_url,
    createdAt: c.created_at,
    updatedAt: c.updated_at,
    ownerId: c.owner_id,
    version: c.version,
    endpoints: {
      primary: c.connection_url,
    },
  };
}

// Cluster API
export const clusterApi = {
  list: async (page = 1, pageSize = 10): Promise<PaginatedResponse<Cluster>> => {
    // API returns snake_case format, map to frontend format
    const response = await fetchWithAuth<{
      clusters: ApiCluster[];
      total_count: number;
      page: number;
      page_size: number;
    }>(`/api/v1/clusters?page=${page}&pageSize=${pageSize}`);

    return {
      data: (response.clusters || []).map(transformCluster),
      pagination: {
        page: response.page,
        pageSize: response.page_size,
        total: response.total_count,
        totalPages: Math.ceil(response.total_count / response.page_size),
      },
    };
  },

  get: async (id: string): Promise<ApiResponse<Cluster>> => {
    // API returns snake_case format, map to frontend format
    const response = await fetchWithAuth<{
      cluster: {
        id: string;
        name: string;
        owner_id: string;
        status: string;
        tier: string;
        provider: string;
        region: string;
        version: string;
        node_count: number;
        node_size: string;
        storage_gb: number;
        connection_url: string;
        maintenance_day: string;
        maintenance_hour: number;
        backup_enabled: boolean;
        backup_retention_days: number;
        created_at: string;
        updated_at: string;
      };
    }>(`/api/v1/clusters/${id}`);

    const c = response.cluster;
    return {
      data: {
        id: c.id,
        name: c.name,
        status: c.status as Cluster["status"],
        config: {
          tier: c.tier as Cluster["config"]["tier"],
          provider: c.provider as Cluster["config"]["provider"],
          region: c.region,
          nodeCount: c.node_count,
          storageGb: c.storage_gb,
          highAvailability: false,
          backupEnabled: c.backup_enabled,
          backupRetentionDays: c.backup_retention_days,
          maintenanceWindow: {
            dayOfWeek: ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"].indexOf(c.maintenance_day),
            hourUtc: c.maintenance_hour,
          },
        },
        connectionString: c.connection_url,
        createdAt: c.created_at,
        updatedAt: c.updated_at,
        ownerId: c.owner_id,
        version: c.version,
        endpoints: {
          primary: c.connection_url,
        },
      },
    };
  },

  create: async (data: CreateClusterForm): Promise<ApiResponse<Cluster>> => {
    const response = await fetchWithAuth<{ cluster: ApiCluster }>("/api/v1/clusters", {
      method: "POST",
      body: JSON.stringify(data),
    });
    return { data: transformCluster(response.cluster) };
  },

  update: async (id: string, data: UpdateClusterForm): Promise<ApiResponse<Cluster>> => {
    const response = await fetchWithAuth<{ cluster: ApiCluster }>(`/api/v1/clusters/${id}`, {
      method: "PATCH",
      body: JSON.stringify(data),
    });
    return { data: transformCluster(response.cluster) };
  },

  delete: async (id: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/clusters/${id}`, {
      method: "DELETE",
    });
  },

  start: async (id: string): Promise<ApiResponse<Cluster>> => {
    const response = await fetchWithAuth<{ cluster: ApiCluster }>(`/api/v1/clusters/${id}/start`, {
      method: "POST",
    });
    return { data: transformCluster(response.cluster) };
  },

  stop: async (id: string): Promise<ApiResponse<Cluster>> => {
    const response = await fetchWithAuth<{ cluster: ApiCluster }>(`/api/v1/clusters/${id}/stop`, {
      method: "POST",
    });
    return { data: transformCluster(response.cluster) };
  },

  restart: async (id: string): Promise<ApiResponse<Cluster>> => {
    const response = await fetchWithAuth<{ cluster: ApiCluster }>(`/api/v1/clusters/${id}/restart`, {
      method: "POST",
    });
    return { data: transformCluster(response.cluster) };
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

// Orochi DB APIs
export const orochiApi = {
  // Sharding API
  getShardDistribution: async (clusterId: string): Promise<ApiResponse<ShardDistribution>> => {
    return fetchWithAuth<ApiResponse<ShardDistribution>>(
      `/api/v1/clusters/${clusterId}/sharding`
    );
  },

  triggerRebalance: async (clusterId: string): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/sharding/rebalance`,
      { method: "POST" }
    );
  },

  // Time-Series API
  getTimeSeriesMetrics: async (clusterId: string): Promise<ApiResponse<TimeSeriesMetrics>> => {
    return fetchWithAuth<ApiResponse<TimeSeriesMetrics>>(
      `/api/v1/clusters/${clusterId}/timeseries`
    );
  },

  compressChunks: async (
    clusterId: string,
    chunkIds: string[]
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/timeseries/compress`,
      {
        method: "POST",
        body: JSON.stringify({ chunkIds }),
      }
    );
  },

  // Columnar Storage API
  getColumnarStats: async (clusterId: string): Promise<ApiResponse<ColumnarStats>> => {
    return fetchWithAuth<ApiResponse<ColumnarStats>>(
      `/api/v1/clusters/${clusterId}/columnar`
    );
  },

  convertToColumnar: async (
    clusterId: string,
    tableName: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/columnar/convert`,
      {
        method: "POST",
        body: JSON.stringify({ tableName }),
      }
    );
  },

  // Topology API
  getTopology: async (clusterId: string): Promise<ApiResponse<ClusterTopology>> => {
    return fetchWithAuth<ApiResponse<ClusterTopology>>(
      `/api/v1/clusters/${clusterId}/topology`
    );
  },

  // Pipeline API
  getPipelineMetrics: async (clusterId: string): Promise<ApiResponse<PipelineMetrics>> => {
    return fetchWithAuth<ApiResponse<PipelineMetrics>>(
      `/api/v1/clusters/${clusterId}/pipelines`
    );
  },

  pausePipeline: async (
    clusterId: string,
    pipelineId: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pipelines/${pipelineId}/pause`,
      { method: "POST" }
    );
  },

  resumePipeline: async (
    clusterId: string,
    pipelineId: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pipelines/${pipelineId}/resume`,
      { method: "POST" }
    );
  },

  // CDC API
  getCDCMetrics: async (clusterId: string): Promise<ApiResponse<CDCMetrics>> => {
    return fetchWithAuth<ApiResponse<CDCMetrics>>(
      `/api/v1/clusters/${clusterId}/cdc`
    );
  },

  pauseCDCSubscription: async (
    clusterId: string,
    subscriptionId: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/cdc/${subscriptionId}/pause`,
      { method: "POST" }
    );
  },

  resumeCDCSubscription: async (
    clusterId: string,
    subscriptionId: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/cdc/${subscriptionId}/resume`,
      { method: "POST" }
    );
  },
};

// Export error class
export { ApiError };
