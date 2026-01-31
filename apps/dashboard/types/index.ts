// User and Authentication Types
export type UserRole = "admin" | "member" | "viewer";

export interface User {
  id: string;
  email: string;
  name: string;
  role: UserRole;
  createdAt: string;
  updatedAt: string;
  emailVerified: boolean;
  avatar?: string;
}

export interface AuthTokens {
  accessToken: string;
  refreshToken: string;
  expiresAt: number;
}

export interface AuthState {
  user: User | null;
  tokens: AuthTokens | null;
  isAuthenticated: boolean;
  isLoading: boolean;
}

export interface LoginCredentials {
  email: string;
  password: string;
}

export interface RegisterCredentials {
  email: string;
  password: string;
  name: string;
  organizationName?: string;
  inviteToken?: string;
}

// Organization Types
export type OrganizationRole = "owner" | "admin" | "member" | "viewer";

export interface Organization {
  id: string;
  name: string;
  slug: string;
  createdAt: string;
  updatedAt: string;
}

export interface OrganizationMember {
  id: string;
  userId: string;
  organizationId: string;
  role: OrganizationRole;
  email: string;
  name: string;
  joinedAt: string;
}

export interface OrganizationInvite {
  id: string;
  organizationId: string;
  organizationName: string;
  email: string;
  role: OrganizationRole;
  invitedByName: string;
  token: string;
  expiresAt: string;
  acceptedAt?: string;
  createdAt: string;
}

export interface CreateInviteRequest {
  email: string;
  role: OrganizationRole;
}

export interface CreateOrganizationRequest {
  name: string;
}

// Cluster Types
export type ClusterStatus = "creating" | "running" | "stopped" | "error" | "deleting" | "updating";
export type ClusterTier = "free" | "standard" | "professional" | "enterprise";
export type CloudProvider = "aws" | "gcp" | "azure";
export type Region = string;

// Connection Pooler Configuration
export type PoolerMode = "transaction" | "session";

export interface PoolerConfig {
  enabled: boolean;
  mode: PoolerMode;
  maxClientConnections: number;
  defaultPoolSize: number;
  maxPoolSize: number;
  idleTimeout: number;  // seconds
}

// Compute Autoscaling Configuration
export interface AutoscaleConfig {
  enabled: boolean;
  minComputeUnits: number;  // 0.25 to 8 CU (like NeonDB)
  maxComputeUnits: number;
  scaleToZero: boolean;
  scaleToZeroDelayMinutes: number;  // Time before scaling to zero
}

// Network/IP Allow List
export interface IpAllowEntry {
  id: string;
  cidr: string;  // IP or CIDR range
  description: string;
  createdAt: string;
}

export interface NetworkConfig {
  ipAllowList: IpAllowEntry[];
  sslRequired: boolean;
  publicAccess: boolean;  // If false, only VPC peering allowed
}

// Read Replica Configuration
export interface ReadReplicaConfig {
  enabled: boolean;
  replicaCount: number;
  regions: string[];  // Additional regions for read replicas
}

export interface ClusterConfig {
  tier: ClusterTier;
  provider: CloudProvider;
  region: Region;
  nodeCount: number;
  storageGb: number;
  highAvailability: boolean;
  backupEnabled: boolean;
  backupRetentionDays: number;
  maintenanceWindow?: {
    dayOfWeek: number;
    hourUtc: number;
  };
  // Advanced settings
  pooler?: PoolerConfig;
  autoscale?: AutoscaleConfig;
  network?: NetworkConfig;
  readReplicas?: ReadReplicaConfig;
}

export interface Cluster {
  id: string;
  name: string;
  status: ClusterStatus;
  config: ClusterConfig;
  connectionString: string;
  poolerUrl?: string;  // PgBouncer connection pooler URL
  poolerEnabled: boolean;  // Whether connection pooling is enabled
  createdAt: string;
  updatedAt: string;
  ownerId: string;
  organizationId?: string;  // Optional organization for team-based isolation
  version: string;
  endpoints: {
    primary: string;
    replica?: string;
    pooler?: string;  // PgBouncer endpoint
  };
}

export interface ClusterMetrics {
  clusterId: string;
  timestamp: string;
  cpu: {
    usagePercent: number;
    coreCount: number;
  };
  memory: {
    usedBytes: number;
    totalBytes: number;
    usagePercent: number;
  };
  storage: {
    usedBytes: number;
    totalBytes: number;
    usagePercent: number;
  };
  connections: {
    active: number;
    idle: number;
    max: number;
  };
  queries: {
    queriesPerSecond: number;
    avgLatencyMs: number;
    slowQueries: number;
  };
  replication?: {
    lagBytes: number;
    lagMs: number;
  };
}

export interface ClusterMetricsHistory {
  clusterId: string;
  period: "1h" | "6h" | "24h" | "7d" | "30d";
  dataPoints: Array<{
    timestamp: string;
    cpuPercent: number;
    memoryPercent: number;
    storagePercent: number;
    connections: number;
    qps: number;
    latencyMs: number;
  }>;
}

// API Response Types
export interface ApiResponse<T> {
  data: T;
  message?: string;
}

export interface ApiError {
  error: string;
  code: string;
  details?: Record<string, string[]>;
}

export interface PaginatedResponse<T> {
  data: T[];
  pagination: {
    page: number;
    pageSize: number;
    total: number;
    totalPages: number;
  };
}

// Form Types
export interface CreateClusterForm {
  name: string;
  tier: ClusterTier;
  provider: CloudProvider;
  region: Region;
  nodeCount: number;
  storageGb: number;
  highAvailability: boolean;
  backupEnabled: boolean;
  poolerEnabled?: boolean;  // Enable PgBouncer connection pooling
  organizationId?: string;  // Optional organization for team-based isolation
}

export interface UpdateClusterForm {
  name?: string;
  nodeCount?: number;
  storageGb?: number;
  backupEnabled?: boolean;
  backupRetentionDays?: number;
  maintenanceWindow?: {
    dayOfWeek: number;
    hourUtc: number;
  };
  // Connection pooler settings
  pooler?: Partial<PoolerConfig>;
  // Autoscaling settings
  autoscale?: Partial<AutoscaleConfig>;
  // Network settings
  network?: Partial<NetworkConfig>;
  // Read replica settings
  readReplicas?: Partial<ReadReplicaConfig>;
}

export interface UpdateUserForm {
  name?: string;
  email?: string;
  currentPassword?: string;
  newPassword?: string;
}

// UI Types
export interface NavItem {
  title: string;
  href: string;
  icon?: string;
  disabled?: boolean;
  external?: boolean;
  badge?: string;
}

export interface Toast {
  id: string;
  title: string;
  description?: string;
  type: "default" | "success" | "error" | "warning";
  duration?: number;
}

// Provider/Region Options
export interface ProviderOption {
  id: CloudProvider;
  name: string;
  regions: RegionOption[];
}

export interface RegionOption {
  id: string;
  name: string;
  location: string;
}

/**
 * Default provider options used as fallback when API is unavailable.
 * For dynamic data, use the useConfig() or useProviders() hook from hooks/use-config.ts
 */
export const PROVIDERS: ProviderOption[] = [
  {
    id: "aws",
    name: "Amazon Web Services",
    regions: [
      { id: "us-east-1", name: "US East (N. Virginia)", location: "Virginia, USA" },
      { id: "us-west-2", name: "US West (Oregon)", location: "Oregon, USA" },
      { id: "eu-west-1", name: "EU (Ireland)", location: "Dublin, Ireland" },
      { id: "eu-central-1", name: "EU (Frankfurt)", location: "Frankfurt, Germany" },
      { id: "ap-southeast-1", name: "Asia Pacific (Singapore)", location: "Singapore" },
      { id: "ap-northeast-1", name: "Asia Pacific (Tokyo)", location: "Tokyo, Japan" },
    ],
  },
  {
    id: "gcp",
    name: "Google Cloud Platform",
    regions: [
      { id: "us-central1", name: "US Central (Iowa)", location: "Iowa, USA" },
      { id: "us-east1", name: "US East (South Carolina)", location: "South Carolina, USA" },
      { id: "europe-west1", name: "Europe West (Belgium)", location: "Belgium" },
      { id: "asia-east1", name: "Asia East (Taiwan)", location: "Taiwan" },
    ],
  },
  {
    id: "azure",
    name: "Microsoft Azure",
    regions: [
      { id: "eastus", name: "East US", location: "Virginia, USA" },
      { id: "westus2", name: "West US 2", location: "Washington, USA" },
      { id: "westeurope", name: "West Europe", location: "Netherlands" },
      { id: "southeastasia", name: "Southeast Asia", location: "Singapore" },
    ],
  },
];

/**
 * Default tier options used as fallback when API is unavailable.
 * For dynamic data, use the useConfig() or useTiers() hook from hooks/use-config.ts
 */
export const TIER_OPTIONS: Array<{
  id: ClusterTier;
  name: string;
  description: string;
  price: string;
  features: string[];
}> = [
  {
    id: "free",
    name: "Free",
    description: "Perfect for learning and prototyping",
    price: "$0/month",
    features: ["1 vCPU", "1 GB RAM", "5 GB Storage", "Shared resources"],
  },
  {
    id: "standard",
    name: "Standard",
    description: "For small production workloads",
    price: "$29/month",
    features: ["2 vCPU", "4 GB RAM", "50 GB Storage", "Daily backups"],
  },
  {
    id: "professional",
    name: "Professional",
    description: "For growing applications",
    price: "$99/month",
    features: ["4 vCPU", "16 GB RAM", "200 GB Storage", "High availability", "Point-in-time recovery"],
  },
  {
    id: "enterprise",
    name: "Enterprise",
    description: "For mission-critical workloads",
    price: "$499/month",
    features: ["16 vCPU", "64 GB RAM", "1 TB Storage", "Multi-region", "24/7 support", "SLA guarantee"],
  },
];

// Orochi DB Specific Types

// Sharding Types
export interface ShardInfo {
  shardId: number;
  nodeId: string;
  rowCount: number;
  sizeBytes: number;
  replicationFactor: number;
  status: "active" | "rebalancing" | "offline";
  lastModified: string;
}

export interface ShardDistribution {
  clusterId: string;
  totalShards: number;
  hashAlgorithm: string;
  rebalanceStatus: {
    inProgress: boolean;
    progress: number;
    startedAt?: string;
    estimatedCompletion?: string;
  };
  shards: ShardInfo[];
  nodeDistribution: {
    nodeId: string;
    nodeName: string;
    shardCount: number;
    totalRows: number;
    totalSizeBytes: number;
  }[];
}

// Time-Series Types
export type CompressionType = "none" | "lz4" | "zstd" | "delta" | "gorilla" | "dictionary" | "rle";

export interface ChunkInfo {
  chunkId: string;
  tableName: string;
  timeRange: {
    start: string;
    end: string;
  };
  rowCount: number;
  compressedSizeBytes: number;
  uncompressedSizeBytes: number;
  compressionRatio: number;
  compressionType: CompressionType;
  status: "active" | "compressed" | "archived";
}

export interface HypertableInfo {
  tableName: string;
  partitionColumn: string;
  chunkInterval: string;
  totalChunks: number;
  totalRows: number;
  totalSizeBytes: number;
  compressionEnabled: boolean;
  continuousAggregates: string[];
}

export interface TimeSeriesMetrics {
  clusterId: string;
  hypertables: HypertableInfo[];
  recentChunks: ChunkInfo[];
  compressionStats: {
    totalCompressed: number;
    totalUncompressed: number;
    avgCompressionRatio: number;
    compressionByType: Record<CompressionType, number>;
  };
  ingestionRate: {
    rowsPerSecond: number;
    bytesPerSecond: number;
    timestamp: string;
  };
}

// Columnar Storage Types
export interface ColumnChunk {
  columnName: string;
  dataType: string;
  rowCount: number;
  nullCount: number;
  compressedSizeBytes: number;
  uncompressedSizeBytes: number;
  compressionType: CompressionType;
  minValue?: string | number;
  maxValue?: string | number;
}

export interface StripeInfo {
  stripeId: string;
  tableName: string;
  rowCount: number;
  columnChunks: ColumnChunk[];
  totalCompressedSize: number;
  totalUncompressedSize: number;
  compressionRatio: number;
  createdAt: string;
}

export interface ColumnarStats {
  clusterId: string;
  totalTables: number;
  totalStripes: number;
  totalRows: number;
  totalCompressedSize: number;
  totalUncompressedSize: number;
  avgCompressionRatio: number;
  compressionByType: Record<CompressionType, { count: number; sizeBytes: number }>;
  recentStripes: StripeInfo[];
}

// Cluster Topology Types
export type StorageTier = "hot" | "warm" | "cold" | "frozen";

export interface NodeHealth {
  nodeId: string;
  nodeName: string;
  role: "coordinator" | "worker" | "both";
  status: "healthy" | "degraded" | "unhealthy" | "unknown";
  cpu: {
    usagePercent: number;
    cores: number;
  };
  memory: {
    usedBytes: number;
    totalBytes: number;
    usagePercent: number;
  };
  disk: {
    usedBytes: number;
    totalBytes: number;
    usagePercent: number;
  };
  connections: number;
  lastHeartbeat: string;
}

export interface RaftStatus {
  nodeId: string;
  state: "leader" | "follower" | "candidate";
  term: number;
  votedFor?: string;
  commitIndex: number;
  lastApplied: number;
  leader?: string;
  followers: string[];
}

export interface TierDistribution {
  tier: StorageTier;
  sizeBytes: number;
  rowCount: number;
  tableCount: number;
  ageThreshold: string;
}

export interface ClusterTopology {
  clusterId: string;
  nodes: NodeHealth[];
  raftStatus: RaftStatus[];
  consensus: {
    healthy: boolean;
    leader: string;
    quorumSize: number;
    lastElection: string;
  };
  tierDistribution: TierDistribution[];
}

// Pipeline Types
export type PipelineSourceType = "kafka" | "s3" | "filesystem" | "webhook";
export type PipelineStatus = "running" | "paused" | "error" | "stopped";

export interface PipelineSource {
  type: PipelineSourceType;
  config: Record<string, unknown>;
}

export interface Pipeline {
  pipelineId: string;
  name: string;
  source: PipelineSource;
  targetTable: string;
  status: PipelineStatus;
  createdAt: string;
  lastProcessed?: string;
  metrics: {
    recordsProcessed: number;
    bytesProcessed: number;
    errorCount: number;
    throughputRecordsPerSec: number;
    throughputBytesPerSec: number;
    lagMs?: number;
  };
}

export interface PipelineMetrics {
  clusterId: string;
  pipelines: Pipeline[];
  totalRecordsProcessed: number;
  totalBytesProcessed: number;
  totalErrors: number;
  avgThroughputRecordsPerSec: number;
}

// CDC Types
export type CDCEventType = "insert" | "update" | "delete" | "truncate";

export interface CDCSubscription {
  subscriptionId: string;
  name: string;
  sourceTables: string[];
  destination: {
    type: "kafka" | "webhook" | "s3";
    config: Record<string, unknown>;
  };
  status: "active" | "paused" | "error";
  createdAt: string;
  metrics: {
    eventsPublished: number;
    bytesPublished: number;
    lagMs: number;
    errorCount: number;
    throughputEventsPerSec: number;
  };
}

export interface CDCMetrics {
  clusterId: string;
  subscriptions: CDCSubscription[];
  totalEventsPublished: number;
  totalBytesPublished: number;
  avgLagMs: number;
  eventsByType: Record<CDCEventType, number>;
}

// Admin Types
export interface AdminUser {
  id: string;
  email: string;
  name: string;
  role: UserRole;
  active: boolean;
  createdAt: string;
  updatedAt: string;
  lastLoginAt?: string;
  clusterCount: number;
}

export interface AdminCluster {
  id: string;
  name: string;
  ownerId: string;
  ownerEmail: string;
  ownerName: string;
  organizationId?: string;  // Optional organization for team-based isolation
  organizationName?: string;  // Organization name if part of a team
  status: ClusterStatus;
  tier: ClusterTier;
  provider: CloudProvider;
  region: string;
  version: string;
  nodeCount: number;
  nodeSize: string;
  storageGb: number;
  connectionUrl: string;
  poolerUrl?: string;  // PgBouncer connection pooler URL
  poolerEnabled: boolean;  // Whether connection pooling is enabled
  maintenanceDay: string;
  maintenanceHour: number;
  backupEnabled: boolean;
  backupRetentionDays: number;
  createdAt: string;
  updatedAt: string;
  deletedAt?: string;
}

export interface AdminStats {
  totalUsers: number;
  activeUsers: number;
  totalClusters: number;
  runningClusters: number;
  totalOrganizations: number;
  updatedAt: string;
}

export interface AdminUserListResponse {
  users: AdminUser[];
  totalCount: number;
  page: number;
  pageSize: number;
}

export interface AdminClusterListResponse {
  clusters: AdminCluster[];
  totalCount: number;
  page: number;
  pageSize: number;
}

// Data Browser Types
export interface TableInfo {
  schema: string;
  name: string;
  type: "table" | "view" | "materialized_view";
  rowEstimate: number;
  sizeBytes: number;
  sizeHuman: string;
  columns: number;
  hasPrimaryKey: boolean;
  createdAt?: string;
}

export interface ColumnInfo {
  name: string;
  type: string;
  nullable: boolean;
  defaultValue?: string;
  isPrimaryKey: boolean;
  isForeignKey: boolean;
  fkReference?: string;
  position: number;
}

export interface IndexInfo {
  name: string;
  columns: string[];
  isUnique: boolean;
  isPrimary: boolean;
  type: string;
  sizeBytes: number;
}

export interface ForeignKeyInfo {
  name: string;
  columns: string[];
  referencedTable: string;
  referencedColumns: string[];
}

export interface TableSchema {
  schema: string;
  name: string;
  columns: ColumnInfo[];
  primaryKey: string[];
  indexes: IndexInfo[];
  foreignKeys: ForeignKeyInfo[];
  rowEstimate: number;
}

export interface QueryResult {
  columns: string[];
  columnTypes: string[];
  rows: unknown[][];
  rowCount: number;
  totalCount?: number;
  executionTimeMs: number;
  truncated: boolean;
}

export interface QueryHistoryEntry {
  id: string;
  userId: string;
  clusterId: string;
  queryText: string;
  description?: string;
  executionTimeMs: number;
  rowsAffected: number;
  status: "success" | "error";
  errorMessage?: string;
  createdAt: string;
}

export interface InternalTableStats {
  hypertables: number;
  chunks: number;
  compressedChunks: number;
  shardCount: number;
  totalSizeBytes: number;
  totalSizeHuman: string;
}

export interface ExecuteSQLRequest {
  sql: string;
  readOnly?: boolean;
}

export interface TableDataRequest {
  schema: string;
  table: string;
  limit?: number;
  offset?: number;
  orderBy?: string;
  orderDir?: "ASC" | "DESC";
}

// Database Branch Types
export type BranchMethod = "volumeSnapshot" | "clone" | "pg_basebackup" | "pitr";
export type BranchStatus = "creating" | "ready" | "failed" | "deleting" | "promoting";

export interface Branch {
  id: string;
  name: string;
  parentClusterId: string;
  parentCluster: string;
  status: BranchStatus;
  method: BranchMethod;
  connectionString?: string;
  poolerConnection?: string;
  createdAt: string;
  labels?: Record<string, string>;
}

export interface CreateBranchForm {
  name: string;
  method?: BranchMethod;
  pointInTime?: string;
  lsn?: string;
  instances?: number;
  inherit?: boolean;
}

export interface BranchListResponse {
  branches: Branch[];
  totalCount: number;
}

// Re-export pooler types
export * from "./pooler";
