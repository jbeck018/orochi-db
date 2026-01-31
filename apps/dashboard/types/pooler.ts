// PgDog Pooler Types

export type PoolerMode = "transaction" | "session" | "statement";

export interface PoolerConfig {
  enabled: boolean;
  mode: PoolerMode;
  maxPoolSize: number;
  minPoolSize: number;
  idleTimeoutSeconds: number;
  readWriteSplit: boolean;
  shardingEnabled: boolean;
  shardCount?: number;
  // PgDog-specific settings
  loadBalancingMode?: "round_robin" | "least_connections" | "random";
  healthCheckIntervalSeconds?: number;
  connectionTimeoutMs?: number;
  queryTimeoutMs?: number;
}

export interface PoolerStats {
  activeConnections: number;
  idleConnections: number;
  waitingClients: number;
  totalQueries: number;
  queriesPerSecond: number;
  avgLatencyMs: number;
  p99LatencyMs: number;
  poolUtilization: number;
  // Extended stats
  serverConnections?: number;
  maxServerConnections?: number;
  transactionsPerSecond?: number;
  bytesReceived?: number;
  bytesSent?: number;
}

export interface PoolerStatus {
  clusterId: string;
  healthy: boolean;
  config: PoolerConfig;
  stats: PoolerStats;
  replicas: number;
  readyReplicas: number;
  lastUpdated: string;
  version?: string;
  uptime?: number;
}

export interface PoolerClient {
  id: string;
  database: string;
  user: string;
  state: "active" | "idle" | "waiting" | "transaction";
  applicationName?: string;
  clientAddress: string;
  connectTime: string;
  lastQueryTime?: string;
  queryDurationMs?: number;
  transactionDurationMs?: number;
  waitDurationMs?: number;
}

export interface PoolerPool {
  database: string;
  user: string;
  mode: PoolerMode;
  clientConnections: number;
  serverConnections: number;
  poolSize: number;
  freeServers: number;
  usedServers: number;
  waitingClients: number;
}

export interface PoolerServer {
  id: string;
  database: string;
  user: string;
  host: string;
  port: number;
  state: "active" | "idle" | "used" | "tested";
  role: "primary" | "replica";
  connectTime: string;
  requestCount: number;
  bytesReceived: number;
  bytesSent: number;
}

export interface PoolerStatsHistory {
  clusterId: string;
  period: "1h" | "6h" | "24h" | "7d";
  dataPoints: Array<{
    timestamp: string;
    activeConnections: number;
    idleConnections: number;
    waitingClients: number;
    queriesPerSecond: number;
    avgLatencyMs: number;
    poolUtilization: number;
  }>;
}

// API Request/Response types
export interface UpdatePoolerConfigRequest {
  enabled?: boolean;
  mode?: PoolerMode;
  maxPoolSize?: number;
  minPoolSize?: number;
  idleTimeoutSeconds?: number;
  readWriteSplit?: boolean;
  shardingEnabled?: boolean;
  shardCount?: number;
  loadBalancingMode?: "round_robin" | "least_connections" | "random";
  healthCheckIntervalSeconds?: number;
  connectionTimeoutMs?: number;
  queryTimeoutMs?: number;
}

export interface PoolerClientsResponse {
  clients: PoolerClient[];
  totalCount: number;
  page: number;
  pageSize: number;
}

export interface PoolerPoolsResponse {
  pools: PoolerPool[];
}

export interface PoolerServersResponse {
  servers: PoolerServer[];
}
