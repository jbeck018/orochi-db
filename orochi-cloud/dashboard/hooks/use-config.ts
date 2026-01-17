"use client";

import * as React from "react";
import { configApi, type ProviderOption, type TierOption, type SystemHealth } from "@/lib/api";
import { PROVIDERS as DEFAULT_PROVIDERS, TIER_OPTIONS as DEFAULT_TIER_OPTIONS } from "@/types";

interface ConfigState {
  providers: ProviderOption[];
  tiers: TierOption[];
  systemHealth: SystemHealth | null;
  isLoading: boolean;
  error: string | null;
}

interface UseConfigReturn extends ConfigState {
  refetch: () => Promise<void>;
}

export function useConfig(): UseConfigReturn {
  const [state, setState] = React.useState<ConfigState>({
    providers: DEFAULT_PROVIDERS.map(p => ({
      id: p.id,
      name: p.name,
      regions: p.regions,
    })),
    tiers: DEFAULT_TIER_OPTIONS.map(t => ({
      id: t.id,
      name: t.name,
      description: t.description,
      price: t.price,
      features: t.features,
      limits: {
        vcpu: t.id === "free" ? 1 : t.id === "standard" ? 2 : t.id === "professional" ? 4 : 16,
        memoryGb: t.id === "free" ? 1 : t.id === "standard" ? 4 : t.id === "professional" ? 16 : 64,
        storageGb: t.id === "free" ? 5 : t.id === "standard" ? 50 : t.id === "professional" ? 200 : 1000,
      },
    })),
    systemHealth: null,
    isLoading: true,
    error: null,
  });

  // Track mounted state to prevent state updates after unmount
  const isMountedRef = React.useRef(true);

  const fetchConfig = React.useCallback(async () => {
    setState(prev => ({ ...prev, isLoading: true, error: null }));

    try {
      const [providersRes, tiersRes, healthRes] = await Promise.allSettled([
        configApi.getProviders(),
        configApi.getTiers(),
        configApi.getSystemHealth(),
      ]);

      // Check if still mounted before updating state
      if (!isMountedRef.current) return;

      setState(prev => ({
        ...prev,
        providers: providersRes.status === "fulfilled" ? providersRes.value.data : prev.providers,
        tiers: tiersRes.status === "fulfilled" ? tiersRes.value.data : prev.tiers,
        systemHealth: healthRes.status === "fulfilled" ? healthRes.value.data : null,
        isLoading: false,
      }));
    } catch (error) {
      // Check if still mounted before updating state
      if (!isMountedRef.current) return;

      setState(prev => ({
        ...prev,
        isLoading: false,
        error: error instanceof Error ? error.message : "Failed to load configuration",
      }));
    }
  }, []);

  React.useEffect(() => {
    isMountedRef.current = true;
    fetchConfig();
    return () => {
      isMountedRef.current = false;
    };
  }, [fetchConfig]);

  return {
    ...state,
    refetch: fetchConfig,
  };
}

export function useProviders(): { providers: ProviderOption[]; isLoading: boolean } {
  const { providers, isLoading } = useConfig();
  return { providers, isLoading };
}

export function useTiers(): { tiers: TierOption[]; isLoading: boolean } {
  const { tiers, isLoading } = useConfig();
  return { tiers, isLoading };
}

export function useSystemHealth(): { health: SystemHealth | null; isLoading: boolean; refetch: () => Promise<void> } {
  const { systemHealth, isLoading, refetch } = useConfig();
  return { health: systemHealth, isLoading, refetch };
}
