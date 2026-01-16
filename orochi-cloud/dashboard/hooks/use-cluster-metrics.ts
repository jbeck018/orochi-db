"use client";

import * as React from "react";
import { clusterApi } from "@/lib/api";
import type { ClusterMetrics, ClusterMetricsHistory } from "@/types";

interface UseClusterMetricsOptions {
  clusterId: string;
  enabled?: boolean;
  pollInterval?: number;
}

interface UseClusterMetricsReturn {
  metrics: ClusterMetrics | null;
  metricsHistory: ClusterMetricsHistory | null;
  isLoading: boolean;
  error: Error | null;
  refetch: () => Promise<void>;
  setHistoryPeriod: (period: "1h" | "6h" | "24h" | "7d" | "30d") => Promise<void>;
}

export function useClusterMetrics({
  clusterId,
  enabled = true,
  pollInterval = 30000,
}: UseClusterMetricsOptions): UseClusterMetricsReturn {
  const [metrics, setMetrics] = React.useState<ClusterMetrics | null>(null);
  const [metricsHistory, setMetricsHistory] = React.useState<ClusterMetricsHistory | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);
  const [error, setError] = React.useState<Error | null>(null);
  const [period, setPeriod] = React.useState<"1h" | "6h" | "24h" | "7d" | "30d">("24h");

  const fetchMetrics = React.useCallback(async () => {
    if (!enabled) return;

    try {
      const [metricsResponse, historyResponse] = await Promise.all([
        clusterApi.getMetrics(clusterId),
        clusterApi.getMetricsHistory(clusterId, period),
      ]);
      setMetrics(metricsResponse.data);
      setMetricsHistory(historyResponse.data);
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err : new Error("Failed to fetch metrics"));
    } finally {
      setIsLoading(false);
    }
  }, [clusterId, enabled, period]);

  React.useEffect(() => {
    fetchMetrics();

    if (enabled && pollInterval > 0) {
      const interval = setInterval(fetchMetrics, pollInterval);
      return () => clearInterval(interval);
    }
  }, [fetchMetrics, enabled, pollInterval]);

  const setHistoryPeriod = async (newPeriod: "1h" | "6h" | "24h" | "7d" | "30d"): Promise<void> => {
    setPeriod(newPeriod);
    try {
      const response = await clusterApi.getMetricsHistory(clusterId, newPeriod);
      setMetricsHistory(response.data);
    } catch (err) {
      setError(err instanceof Error ? err : new Error("Failed to fetch metrics history"));
    }
  };

  return {
    metrics,
    metricsHistory,
    isLoading,
    error,
    refetch: fetchMetrics,
    setHistoryPeriod,
  };
}
