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
  AdminStats,
  AdminUser,
  AdminCluster,
  AdminUserListResponse,
  AdminClusterListResponse,
  UserRole,
  TableInfo,
  TableSchema,
  QueryResult,
  QueryHistoryEntry,
  InternalTableStats,
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
  organization_id?: string;
  status: string;
  tier: string;
  provider: string;
  region: string;
  version: string;
  node_count: number;
  node_size: string;
  storage_gb: number;
  connection_url: string;
  pooler_url?: string;
  pooler_enabled: boolean;
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
    poolerUrl: c.pooler_url,
    poolerEnabled: c.pooler_enabled,
    createdAt: c.created_at,
    updatedAt: c.updated_at,
    ownerId: c.owner_id,
    organizationId: c.organization_id,
    version: c.version,
    endpoints: {
      primary: c.connection_url,
      pooler: c.pooler_url,
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
      cluster: ApiCluster;
    }>(`/api/v1/clusters/${id}`);

    return {
      data: transformCluster(response.cluster),
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

// Admin API Types from backend
interface ApiAdminUser {
  id: string;
  email: string;
  name: string;
  role: string;
  active: boolean;
  created_at: string;
  updated_at: string;
  last_login_at?: string;
  cluster_count: number;
}

interface ApiAdminCluster {
  id: string;
  name: string;
  owner_id: string;
  owner_email: string;
  owner_name: string;
  organization_id?: string;
  organization_name?: string;
  status: string;
  tier: string;
  provider: string;
  region: string;
  version: string;
  node_count: number;
  node_size: string;
  storage_gb: number;
  connection_url: string;
  pooler_url?: string;
  pooler_enabled: boolean;
  maintenance_day: string;
  maintenance_hour: number;
  backup_enabled: boolean;
  backup_retention_days: number;
  created_at: string;
  updated_at: string;
  deleted_at?: string;
}

function transformAdminUser(u: ApiAdminUser): AdminUser {
  return {
    id: u.id,
    email: u.email,
    name: u.name,
    role: u.role as UserRole,
    active: u.active,
    createdAt: u.created_at,
    updatedAt: u.updated_at,
    lastLoginAt: u.last_login_at,
    clusterCount: u.cluster_count,
  };
}

function transformAdminCluster(c: ApiAdminCluster): AdminCluster {
  return {
    id: c.id,
    name: c.name,
    ownerId: c.owner_id,
    ownerEmail: c.owner_email,
    ownerName: c.owner_name,
    organizationId: c.organization_id,
    organizationName: c.organization_name,
    status: c.status as AdminCluster["status"],
    tier: c.tier as AdminCluster["tier"],
    provider: c.provider as AdminCluster["provider"],
    region: c.region,
    version: c.version,
    nodeCount: c.node_count,
    nodeSize: c.node_size,
    storageGb: c.storage_gb,
    connectionUrl: c.connection_url,
    poolerUrl: c.pooler_url,
    poolerEnabled: c.pooler_enabled,
    maintenanceDay: c.maintenance_day,
    maintenanceHour: c.maintenance_hour,
    backupEnabled: c.backup_enabled,
    backupRetentionDays: c.backup_retention_days,
    createdAt: c.created_at,
    updatedAt: c.updated_at,
    deletedAt: c.deleted_at,
  };
}

// Admin API
export const adminApi = {
  getStats: async (): Promise<ApiResponse<AdminStats>> => {
    const response = await fetchWithAuth<{
      success: boolean;
      data: {
        total_users: number;
        active_users: number;
        total_clusters: number;
        running_clusters: number;
        total_organizations: number;
        updated_at: string;
      };
    }>("/api/v1/admin/stats");

    return {
      data: {
        totalUsers: response.data.total_users,
        activeUsers: response.data.active_users,
        totalClusters: response.data.total_clusters,
        runningClusters: response.data.running_clusters,
        totalOrganizations: response.data.total_organizations,
        updatedAt: response.data.updated_at,
      },
    };
  },

  listUsers: async (
    page = 1,
    pageSize = 20,
    search?: string
  ): Promise<AdminUserListResponse> => {
    const params = new URLSearchParams({
      page: page.toString(),
      page_size: pageSize.toString(),
    });
    if (search) {
      params.set("search", search);
    }

    const response = await fetchWithAuth<{
      success: boolean;
      data: {
        users: ApiAdminUser[];
        total_count: number;
        page: number;
        page_size: number;
      };
    }>(`/api/v1/admin/users?${params.toString()}`);

    return {
      users: response.data.users.map(transformAdminUser),
      totalCount: response.data.total_count,
      page: response.data.page,
      pageSize: response.data.page_size,
    };
  },

  getUser: async (id: string): Promise<ApiResponse<AdminUser>> => {
    const response = await fetchWithAuth<{
      success: boolean;
      data: ApiAdminUser;
    }>(`/api/v1/admin/users/${id}`);

    return {
      data: transformAdminUser(response.data),
    };
  },

  updateUserRole: async (id: string, role: UserRole): Promise<void> => {
    await fetchWithAuth<{ success: boolean }>(`/api/v1/admin/users/${id}/role`, {
      method: "PATCH",
      body: JSON.stringify({ role }),
    });
  },

  setUserActive: async (id: string, active: boolean): Promise<void> => {
    await fetchWithAuth<{ success: boolean }>(`/api/v1/admin/users/${id}/active`, {
      method: "PATCH",
      body: JSON.stringify({ active }),
    });
  },

  listClusters: async (
    page = 1,
    pageSize = 20,
    status?: string
  ): Promise<AdminClusterListResponse> => {
    const params = new URLSearchParams({
      page: page.toString(),
      page_size: pageSize.toString(),
    });
    if (status) {
      params.set("status", status);
    }

    const response = await fetchWithAuth<{
      success: boolean;
      data: {
        clusters: ApiAdminCluster[];
        total_count: number;
        page: number;
        page_size: number;
      };
    }>(`/api/v1/admin/clusters?${params.toString()}`);

    return {
      clusters: response.data.clusters.map(transformAdminCluster),
      totalCount: response.data.total_count,
      page: response.data.page,
      pageSize: response.data.page_size,
    };
  },

  getCluster: async (id: string): Promise<ApiResponse<AdminCluster>> => {
    const response = await fetchWithAuth<{
      success: boolean;
      data: ApiAdminCluster;
    }>(`/api/v1/admin/clusters/${id}`);

    return {
      data: transformAdminCluster(response.data),
    };
  },

  forceDeleteCluster: async (id: string): Promise<void> => {
    await fetchWithAuth<{ success: boolean }>(`/api/v1/admin/clusters/${id}/force`, {
      method: "DELETE",
    });
  },
};

// Organization API Types
interface ApiOrganization {
  id: string;
  name: string;
  slug: string;
  created_at: string;
  updated_at: string;
}

interface ApiOrganizationMember {
  id: string;
  user_id: string;
  organization_id: string;
  role: string;
  email: string;
  name: string;
  joined_at: string;
}

interface ApiOrganizationInvite {
  id: string;
  organization_id: string;
  organization_name: string;
  email: string;
  role: string;
  invited_by_name: string;
  token: string;
  expires_at: string;
  accepted_at?: string;
  created_at: string;
}

import type {
  Organization,
  OrganizationMember,
  OrganizationInvite,
  OrganizationRole,
  CreateInviteRequest,
  CreateOrganizationRequest,
} from "@/types";

function transformOrganization(o: ApiOrganization): Organization {
  return {
    id: o.id,
    name: o.name,
    slug: o.slug,
    createdAt: o.created_at,
    updatedAt: o.updated_at,
  };
}

function transformOrganizationMember(m: ApiOrganizationMember): OrganizationMember {
  return {
    id: m.id,
    userId: m.user_id,
    organizationId: m.organization_id,
    role: m.role as OrganizationRole,
    email: m.email,
    name: m.name,
    joinedAt: m.joined_at,
  };
}

function transformOrganizationInvite(i: ApiOrganizationInvite): OrganizationInvite {
  return {
    id: i.id,
    organizationId: i.organization_id,
    organizationName: i.organization_name,
    email: i.email,
    role: i.role as OrganizationRole,
    invitedByName: i.invited_by_name,
    token: i.token,
    expiresAt: i.expires_at,
    acceptedAt: i.accepted_at,
    createdAt: i.created_at,
  };
}

// Organization API
export const organizationApi = {
  list: async (): Promise<Organization[]> => {
    const response = await fetchWithAuth<{
      organizations: ApiOrganization[];
    }>("/api/v1/organizations");
    return (response.organizations ?? []).map(transformOrganization);
  },

  get: async (id: string): Promise<Organization> => {
    const response = await fetchWithAuth<ApiOrganization>(`/api/v1/organizations/${id}`);
    return transformOrganization(response);
  },

  create: async (data: CreateOrganizationRequest): Promise<Organization> => {
    const response = await fetchWithAuth<ApiOrganization>("/api/v1/organizations", {
      method: "POST",
      body: JSON.stringify(data),
    });
    return transformOrganization(response);
  },

  update: async (id: string, data: Partial<CreateOrganizationRequest>): Promise<Organization> => {
    const response = await fetchWithAuth<ApiOrganization>(`/api/v1/organizations/${id}`, {
      method: "PATCH",
      body: JSON.stringify(data),
    });
    return transformOrganization(response);
  },

  delete: async (id: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/organizations/${id}`, {
      method: "DELETE",
    });
  },

  getMembers: async (organizationId: string): Promise<OrganizationMember[]> => {
    const response = await fetchWithAuth<{
      members: ApiOrganizationMember[];
    }>(`/api/v1/organizations/${organizationId}/members`);
    return (response.members ?? []).map(transformOrganizationMember);
  },

  removeMember: async (organizationId: string, memberId: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/organizations/${organizationId}/members/${memberId}`, {
      method: "DELETE",
    });
  },
};

// Invite API
export const inviteApi = {
  // Get invite by token (public - no auth needed)
  getByToken: async (token: string): Promise<OrganizationInvite> => {
    const response = await fetch(`${API_URL}/api/v1/invites/${token}`);
    if (!response.ok) {
      const error = await response.json().catch(() => ({ message: "Invite not found" }));
      throw new ApiError(error.message ?? "Invite not found", response.status);
    }
    const data = await response.json();
    return transformOrganizationInvite(data);
  },

  // Accept invite (auth required)
  accept: async (token: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/invites/${token}/accept`, {
      method: "POST",
    });
  },

  // List my pending invites (auth required)
  listMine: async (): Promise<OrganizationInvite[]> => {
    const response = await fetchWithAuth<{
      invites: ApiOrganizationInvite[];
    }>("/api/v1/invites/me");
    return (response.invites ?? []).map(transformOrganizationInvite);
  },

  // List invites for an organization (admin only)
  listForOrganization: async (organizationId: string): Promise<OrganizationInvite[]> => {
    const response = await fetchWithAuth<{
      invites: ApiOrganizationInvite[];
    }>(`/api/v1/organizations/${organizationId}/invites`);
    return (response.invites ?? []).map(transformOrganizationInvite);
  },

  // Create invite (admin only)
  create: async (organizationId: string, data: CreateInviteRequest): Promise<OrganizationInvite> => {
    const response = await fetchWithAuth<ApiOrganizationInvite>(
      `/api/v1/organizations/${organizationId}/invites`,
      {
        method: "POST",
        body: JSON.stringify(data),
      }
    );
    return transformOrganizationInvite(response);
  },

  // Revoke invite (admin only)
  revoke: async (organizationId: string, inviteId: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/organizations/${organizationId}/invites/${inviteId}`, {
      method: "DELETE",
    });
  },

  // Resend invite (admin only)
  resend: async (organizationId: string, inviteId: string): Promise<void> => {
    await fetchWithAuth<void>(`/api/v1/organizations/${organizationId}/invites/${inviteId}/resend`, {
      method: "POST",
    });
  },
};

// Data Browser API Types (snake_case from backend)
interface ApiTableInfo {
  schema: string;
  name: string;
  type: "table" | "view" | "materialized_view";
  row_estimate: number;
  size_bytes: number;
  size_human: string;
  columns: number;
  has_primary_key: boolean;
  created_at?: string;
}

interface ApiColumnInfo {
  name: string;
  type: string;
  nullable: boolean;
  default_value?: string;
  is_primary_key: boolean;
  is_foreign_key: boolean;
  fk_reference?: string;
  position: number;
}

interface ApiIndexInfo {
  name: string;
  columns: string[];
  is_unique: boolean;
  is_primary: boolean;
  type: string;
  size_bytes: number;
}

interface ApiForeignKeyInfo {
  name: string;
  columns: string[];
  referenced_table: string;
  referenced_columns: string[];
  on_delete: string;
  on_update: string;
}

interface ApiTableSchema {
  schema: string;
  name: string;
  columns: ApiColumnInfo[];
  primary_key: string[];
  indexes: ApiIndexInfo[];
  foreign_keys: ApiForeignKeyInfo[];
  row_estimate: number;
}

interface ApiQueryResult {
  columns: string[];
  column_types: string[];
  rows: unknown[][];
  row_count: number;
  total_count?: number;
  execution_time_ms: number;
  truncated: boolean;
}

interface ApiQueryHistoryEntry {
  id: string;
  user_id: string;
  cluster_id: string;
  query_text: string;
  description?: string;
  execution_time_ms: number;
  rows_affected: number;
  status: "success" | "error";
  error_message?: string;
  created_at: string;
}

interface ApiInternalTableStats {
  hypertables: number;
  chunks: number;
  compressed_chunks: number;
  shard_count: number;
  total_size_bytes: number;
  total_size_human: string;
}

function transformTableInfo(t: ApiTableInfo): TableInfo {
  return {
    schema: t.schema,
    name: t.name,
    type: t.type,
    rowEstimate: t.row_estimate,
    sizeBytes: t.size_bytes,
    sizeHuman: t.size_human,
    columns: t.columns,
    hasPrimaryKey: t.has_primary_key,
    createdAt: t.created_at,
  };
}

function transformTableSchema(s: ApiTableSchema): TableSchema {
  return {
    schema: s.schema,
    name: s.name,
    columns: s.columns.map((c) => ({
      name: c.name,
      type: c.type,
      nullable: c.nullable,
      defaultValue: c.default_value,
      isPrimaryKey: c.is_primary_key,
      isForeignKey: c.is_foreign_key,
      fkReference: c.fk_reference,
      position: c.position,
    })),
    primaryKey: s.primary_key,
    indexes: s.indexes.map((i) => ({
      name: i.name,
      columns: i.columns,
      isUnique: i.is_unique,
      isPrimary: i.is_primary,
      type: i.type,
      sizeBytes: i.size_bytes,
    })),
    foreignKeys: s.foreign_keys.map((f) => ({
      name: f.name,
      columns: f.columns,
      referencedTable: f.referenced_table,
      referencedColumns: f.referenced_columns,
      onDelete: f.on_delete,
      onUpdate: f.on_update,
    })),
    rowEstimate: s.row_estimate,
  };
}

function transformQueryResult(r: ApiQueryResult): QueryResult {
  return {
    columns: r.columns,
    columnTypes: r.column_types,
    rows: r.rows,
    rowCount: r.row_count,
    totalCount: r.total_count,
    executionTimeMs: r.execution_time_ms,
    truncated: r.truncated,
  };
}

function transformQueryHistoryEntry(e: ApiQueryHistoryEntry): QueryHistoryEntry {
  return {
    id: e.id,
    userId: e.user_id,
    clusterId: e.cluster_id,
    queryText: e.query_text,
    description: e.description,
    executionTimeMs: e.execution_time_ms,
    rowsAffected: e.rows_affected,
    status: e.status,
    errorMessage: e.error_message,
    createdAt: e.created_at,
  };
}

function transformInternalStats(s: ApiInternalTableStats): InternalTableStats {
  return {
    hypertables: s.hypertables,
    chunks: s.chunks,
    compressedChunks: s.compressed_chunks,
    shardCount: s.shard_count,
    totalSizeBytes: s.total_size_bytes,
    totalSizeHuman: s.total_size_human,
  };
}

// Data Browser API
export interface DataBrowserTableParams {
  page?: number;
  pageSize?: number;
  sortColumn?: string;
  sortDirection?: "asc" | "desc";
  filters?: Record<string, string>;
}

export const dataBrowserApi = {
  // List all user-visible tables (excludes internal tables)
  listTables: async (clusterId: string): Promise<TableInfo[]> => {
    const response = await fetchWithAuth<{
      tables: ApiTableInfo[];
    }>(`/api/v1/clusters/${clusterId}/data/tables`);
    return (response.tables ?? []).map(transformTableInfo);
  },

  // Get table schema (columns, indexes, foreign keys)
  getTableSchema: async (
    clusterId: string,
    schema: string,
    table: string
  ): Promise<TableSchema> => {
    const response = await fetchWithAuth<ApiTableSchema>(
      `/api/v1/clusters/${clusterId}/data/tables/${encodeURIComponent(schema)}/${encodeURIComponent(table)}`
    );
    return transformTableSchema(response);
  },

  // Get table data with pagination and sorting
  getTableData: async (
    clusterId: string,
    schema: string,
    table: string,
    params: DataBrowserTableParams = {}
  ): Promise<QueryResult> => {
    const queryParams = new URLSearchParams();
    if (params.page) queryParams.set("page", params.page.toString());
    if (params.pageSize) queryParams.set("page_size", params.pageSize.toString());
    if (params.sortColumn) queryParams.set("sort_column", params.sortColumn);
    if (params.sortDirection) queryParams.set("sort_direction", params.sortDirection);
    if (params.filters) {
      Object.entries(params.filters).forEach(([key, value]) => {
        queryParams.set(`filter_${key}`, value);
      });
    }

    const url = `/api/v1/clusters/${clusterId}/data/tables/${encodeURIComponent(schema)}/${encodeURIComponent(table)}/data?${queryParams.toString()}`;
    const response = await fetchWithAuth<ApiQueryResult>(url);
    return transformQueryResult(response);
  },

  // Execute arbitrary SQL query
  executeSQL: async (
    clusterId: string,
    sql: string,
    readOnly = true
  ): Promise<QueryResult> => {
    const response = await fetchWithAuth<ApiQueryResult>(
      `/api/v1/clusters/${clusterId}/data/query`,
      {
        method: "POST",
        body: JSON.stringify({ sql, read_only: readOnly }),
      }
    );
    return transformQueryResult(response);
  },

  // Get query history
  getQueryHistory: async (
    clusterId: string,
    limit = 50
  ): Promise<QueryHistoryEntry[]> => {
    const response = await fetchWithAuth<{
      history: ApiQueryHistoryEntry[];
    }>(`/api/v1/clusters/${clusterId}/data/history?limit=${limit}`);
    return (response.history ?? []).map(transformQueryHistoryEntry);
  },

  // Get internal table statistics (hypertables, shards, etc.)
  getInternalStats: async (clusterId: string): Promise<InternalTableStats> => {
    const response = await fetchWithAuth<ApiInternalTableStats>(
      `/api/v1/clusters/${clusterId}/data/stats`
    );
    return transformInternalStats(response);
  },
};

// Pooler API Types (snake_case from backend)
interface ApiPoolerConfig {
  enabled: boolean;
  mode: string;
  max_pool_size: number;
  min_pool_size: number;
  idle_timeout_seconds: number;
  read_write_split: boolean;
  sharding_enabled: boolean;
  shard_count?: number;
  load_balancing_mode?: string;
  health_check_interval_seconds?: number;
  connection_timeout_ms?: number;
  query_timeout_ms?: number;
}

interface ApiPoolerStats {
  active_connections: number;
  idle_connections: number;
  waiting_clients: number;
  total_queries: number;
  queries_per_second: number;
  avg_latency_ms: number;
  p99_latency_ms: number;
  pool_utilization: number;
  server_connections?: number;
  max_server_connections?: number;
  transactions_per_second?: number;
  bytes_received?: number;
  bytes_sent?: number;
}

interface ApiPoolerStatus {
  cluster_id: string;
  healthy: boolean;
  config: ApiPoolerConfig;
  stats: ApiPoolerStats;
  replicas: number;
  ready_replicas: number;
  last_updated: string;
  version?: string;
  uptime?: number;
}

interface ApiPoolerClient {
  id: string;
  database: string;
  user: string;
  state: string;
  application_name?: string;
  client_address: string;
  connect_time: string;
  last_query_time?: string;
  query_duration_ms?: number;
  transaction_duration_ms?: number;
  wait_duration_ms?: number;
}

interface ApiPoolerPool {
  database: string;
  user: string;
  mode: string;
  client_connections: number;
  server_connections: number;
  pool_size: number;
  free_servers: number;
  used_servers: number;
  waiting_clients: number;
}

interface ApiPoolerStatsHistory {
  cluster_id: string;
  period: string;
  data_points: Array<{
    timestamp: string;
    active_connections: number;
    idle_connections: number;
    waiting_clients: number;
    queries_per_second: number;
    avg_latency_ms: number;
    pool_utilization: number;
  }>;
}

import type {
  PoolerConfig,
  PoolerStats,
  PoolerStatus,
  PoolerClient,
  PoolerPool,
  PoolerClientsResponse,
  PoolerPoolsResponse,
  PoolerStatsHistory,
  PoolerMode,
  UpdatePoolerConfigRequest,
} from "@/types/pooler";

function transformPoolerConfig(c: ApiPoolerConfig): PoolerConfig {
  return {
    enabled: c.enabled,
    mode: c.mode as PoolerMode,
    maxPoolSize: c.max_pool_size,
    minPoolSize: c.min_pool_size,
    idleTimeoutSeconds: c.idle_timeout_seconds,
    readWriteSplit: c.read_write_split,
    shardingEnabled: c.sharding_enabled,
    shardCount: c.shard_count,
    loadBalancingMode: c.load_balancing_mode as PoolerConfig["loadBalancingMode"],
    healthCheckIntervalSeconds: c.health_check_interval_seconds,
    connectionTimeoutMs: c.connection_timeout_ms,
    queryTimeoutMs: c.query_timeout_ms,
  };
}

function transformPoolerStats(s: ApiPoolerStats): PoolerStats {
  return {
    activeConnections: s.active_connections,
    idleConnections: s.idle_connections,
    waitingClients: s.waiting_clients,
    totalQueries: s.total_queries,
    queriesPerSecond: s.queries_per_second,
    avgLatencyMs: s.avg_latency_ms,
    p99LatencyMs: s.p99_latency_ms,
    poolUtilization: s.pool_utilization,
    serverConnections: s.server_connections,
    maxServerConnections: s.max_server_connections,
    transactionsPerSecond: s.transactions_per_second,
    bytesReceived: s.bytes_received,
    bytesSent: s.bytes_sent,
  };
}

function transformPoolerStatus(s: ApiPoolerStatus): PoolerStatus {
  return {
    clusterId: s.cluster_id,
    healthy: s.healthy,
    config: transformPoolerConfig(s.config),
    stats: transformPoolerStats(s.stats),
    replicas: s.replicas,
    readyReplicas: s.ready_replicas,
    lastUpdated: s.last_updated,
    version: s.version,
    uptime: s.uptime,
  };
}

function transformPoolerClient(c: ApiPoolerClient): PoolerClient {
  return {
    id: c.id,
    database: c.database,
    user: c.user,
    state: c.state as PoolerClient["state"],
    applicationName: c.application_name,
    clientAddress: c.client_address,
    connectTime: c.connect_time,
    lastQueryTime: c.last_query_time,
    queryDurationMs: c.query_duration_ms,
    transactionDurationMs: c.transaction_duration_ms,
    waitDurationMs: c.wait_duration_ms,
  };
}

function transformPoolerPool(p: ApiPoolerPool): PoolerPool {
  return {
    database: p.database,
    user: p.user,
    mode: p.mode as PoolerMode,
    clientConnections: p.client_connections,
    serverConnections: p.server_connections,
    poolSize: p.pool_size,
    freeServers: p.free_servers,
    usedServers: p.used_servers,
    waitingClients: p.waiting_clients,
  };
}

function transformPoolerStatsHistory(h: ApiPoolerStatsHistory): PoolerStatsHistory {
  return {
    clusterId: h.cluster_id,
    period: h.period as PoolerStatsHistory["period"],
    dataPoints: h.data_points.map((p) => ({
      timestamp: p.timestamp,
      activeConnections: p.active_connections,
      idleConnections: p.idle_connections,
      waitingClients: p.waiting_clients,
      queriesPerSecond: p.queries_per_second,
      avgLatencyMs: p.avg_latency_ms,
      poolUtilization: p.pool_utilization,
    })),
  };
}

// Pooler API
export const poolerApi = {
  // Get pooler status and stats
  getStatus: async (clusterId: string): Promise<ApiResponse<PoolerStatus>> => {
    const response = await fetchWithAuth<{
      data: ApiPoolerStatus;
    }>(`/api/v1/clusters/${clusterId}/pooler/status`);
    return { data: transformPoolerStatus(response.data) };
  },

  // Get pooler stats with real-time data
  getStats: async (clusterId: string): Promise<ApiResponse<PoolerStats>> => {
    const response = await fetchWithAuth<{
      data: ApiPoolerStats;
    }>(`/api/v1/clusters/${clusterId}/pooler/stats`);
    return { data: transformPoolerStats(response.data) };
  },

  // Get pooler stats history
  getStatsHistory: async (
    clusterId: string,
    period: "1h" | "6h" | "24h" | "7d" = "1h"
  ): Promise<ApiResponse<PoolerStatsHistory>> => {
    const response = await fetchWithAuth<{
      data: ApiPoolerStatsHistory;
    }>(`/api/v1/clusters/${clusterId}/pooler/stats/history?period=${period}`);
    return { data: transformPoolerStatsHistory(response.data) };
  },

  // Get pooler configuration
  getConfig: async (clusterId: string): Promise<ApiResponse<PoolerConfig>> => {
    const response = await fetchWithAuth<{
      data: ApiPoolerConfig;
    }>(`/api/v1/clusters/${clusterId}/pooler/config`);
    return { data: transformPoolerConfig(response.data) };
  },

  // Update pooler configuration
  updateConfig: async (
    clusterId: string,
    config: UpdatePoolerConfigRequest
  ): Promise<ApiResponse<PoolerConfig>> => {
    // Transform to snake_case for API
    const apiConfig: Partial<ApiPoolerConfig> = {};
    if (config.enabled !== undefined) apiConfig.enabled = config.enabled;
    if (config.mode !== undefined) apiConfig.mode = config.mode;
    if (config.maxPoolSize !== undefined) apiConfig.max_pool_size = config.maxPoolSize;
    if (config.minPoolSize !== undefined) apiConfig.min_pool_size = config.minPoolSize;
    if (config.idleTimeoutSeconds !== undefined) apiConfig.idle_timeout_seconds = config.idleTimeoutSeconds;
    if (config.readWriteSplit !== undefined) apiConfig.read_write_split = config.readWriteSplit;
    if (config.shardingEnabled !== undefined) apiConfig.sharding_enabled = config.shardingEnabled;
    if (config.shardCount !== undefined) apiConfig.shard_count = config.shardCount;
    if (config.loadBalancingMode !== undefined) apiConfig.load_balancing_mode = config.loadBalancingMode;
    if (config.healthCheckIntervalSeconds !== undefined) apiConfig.health_check_interval_seconds = config.healthCheckIntervalSeconds;
    if (config.connectionTimeoutMs !== undefined) apiConfig.connection_timeout_ms = config.connectionTimeoutMs;
    if (config.queryTimeoutMs !== undefined) apiConfig.query_timeout_ms = config.queryTimeoutMs;

    const response = await fetchWithAuth<{
      data: ApiPoolerConfig;
    }>(`/api/v1/clusters/${clusterId}/pooler/config`, {
      method: "PATCH",
      body: JSON.stringify(apiConfig),
    });
    return { data: transformPoolerConfig(response.data) };
  },

  // Reload pooler configuration (hot reload)
  reloadConfig: async (clusterId: string): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pooler/reload`,
      { method: "POST" }
    );
  },

  // Get connected clients
  getClients: async (
    clusterId: string,
    page = 1,
    pageSize = 50
  ): Promise<PoolerClientsResponse> => {
    const response = await fetchWithAuth<{
      clients: ApiPoolerClient[];
      total_count: number;
      page: number;
      page_size: number;
    }>(`/api/v1/clusters/${clusterId}/pooler/clients?page=${page}&page_size=${pageSize}`);
    return {
      clients: (response.clients ?? []).map(transformPoolerClient),
      totalCount: response.total_count,
      page: response.page,
      pageSize: response.page_size,
    };
  },

  // Get connection pools
  getPools: async (clusterId: string): Promise<PoolerPoolsResponse> => {
    const response = await fetchWithAuth<{
      pools: ApiPoolerPool[];
    }>(`/api/v1/clusters/${clusterId}/pooler/pools`);
    return {
      pools: (response.pools ?? []).map(transformPoolerPool),
    };
  },

  // Disconnect a client
  disconnectClient: async (
    clusterId: string,
    clientId: string
  ): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pooler/clients/${clientId}`,
      { method: "DELETE" }
    );
  },

  // Pause pooler (stop accepting new connections)
  pause: async (clusterId: string): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pooler/pause`,
      { method: "POST" }
    );
  },

  // Resume pooler
  resume: async (clusterId: string): Promise<ApiResponse<{ message: string }>> => {
    return fetchWithAuth<ApiResponse<{ message: string }>>(
      `/api/v1/clusters/${clusterId}/pooler/resume`,
      { method: "POST" }
    );
  },
};

// Export error class
export { ApiError };
