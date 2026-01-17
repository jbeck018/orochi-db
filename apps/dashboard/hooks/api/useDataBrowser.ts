import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { dataBrowserApi, type DataBrowserTableParams } from "@/lib/api";

// Query keys
export const dataBrowserKeys = {
  all: ["dataBrowser"] as const,
  cluster: (clusterId: string) => [...dataBrowserKeys.all, clusterId] as const,
  tables: (clusterId: string) => [...dataBrowserKeys.cluster(clusterId), "tables"] as const,
  tableSchema: (clusterId: string, schema: string, table: string) =>
    [...dataBrowserKeys.cluster(clusterId), "schema", schema, table] as const,
  tableData: (clusterId: string, schema: string, table: string, params: DataBrowserTableParams) =>
    [...dataBrowserKeys.cluster(clusterId), "data", schema, table, params] as const,
  queryHistory: (clusterId: string) =>
    [...dataBrowserKeys.cluster(clusterId), "history"] as const,
  internalStats: (clusterId: string) =>
    [...dataBrowserKeys.cluster(clusterId), "stats"] as const,
};

// List all user-visible tables
export function useTables(clusterId: string, enabled = true) {
  return useQuery({
    queryKey: dataBrowserKeys.tables(clusterId),
    queryFn: () => dataBrowserApi.listTables(clusterId),
    enabled: !!clusterId && enabled,
    staleTime: 1000 * 60, // 1 minute
  });
}

// Get table schema
export function useTableSchema(
  clusterId: string,
  schema: string,
  table: string,
  enabled = true
) {
  return useQuery({
    queryKey: dataBrowserKeys.tableSchema(clusterId, schema, table),
    queryFn: () => dataBrowserApi.getTableSchema(clusterId, schema, table),
    enabled: !!clusterId && !!schema && !!table && enabled,
    staleTime: 1000 * 60 * 5, // 5 minutes
  });
}

// Get table data with pagination and sorting
export function useTableData(
  clusterId: string,
  schema: string,
  table: string,
  params: DataBrowserTableParams = {},
  enabled = true
) {
  return useQuery({
    queryKey: dataBrowserKeys.tableData(clusterId, schema, table, params),
    queryFn: () => dataBrowserApi.getTableData(clusterId, schema, table, params),
    enabled: !!clusterId && !!schema && !!table && enabled,
    staleTime: 1000 * 30, // 30 seconds
  });
}

// Get query history
export function useQueryHistory(clusterId: string, limit = 50, enabled = true) {
  return useQuery({
    queryKey: dataBrowserKeys.queryHistory(clusterId),
    queryFn: () => dataBrowserApi.getQueryHistory(clusterId, limit),
    enabled: !!clusterId && enabled,
    staleTime: 1000 * 10, // 10 seconds
  });
}

// Get internal table stats
export function useInternalStats(clusterId: string, enabled = true) {
  return useQuery({
    queryKey: dataBrowserKeys.internalStats(clusterId),
    queryFn: () => dataBrowserApi.getInternalStats(clusterId),
    enabled: !!clusterId && enabled,
    staleTime: 1000 * 60, // 1 minute
  });
}

// Execute SQL mutation
export function useExecuteSQL(clusterId: string) {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: ({ sql, readOnly = true }: { sql: string; readOnly?: boolean }) =>
      dataBrowserApi.executeSQL(clusterId, sql, readOnly),
    onSuccess: (_data, variables) => {
      // Invalidate query history
      queryClient.invalidateQueries({
        queryKey: dataBrowserKeys.queryHistory(clusterId),
      });

      // If it was a mutating query, invalidate table data
      if (!variables.readOnly) {
        queryClient.invalidateQueries({
          queryKey: dataBrowserKeys.tables(clusterId),
        });
        // Note: Could be more granular here if we parsed the SQL to know
        // which table was affected, but this is safer
        queryClient.invalidateQueries({
          queryKey: [...dataBrowserKeys.cluster(clusterId), "data"],
        });
      }
    },
  });
}

// Refresh table data
export function useRefreshTableData(clusterId: string) {
  const queryClient = useQueryClient();

  return () => {
    queryClient.invalidateQueries({
      queryKey: dataBrowserKeys.cluster(clusterId),
    });
  };
}

// Prefetch table schema (useful for hover previews)
export function usePrefetchTableSchema(clusterId: string) {
  const queryClient = useQueryClient();

  return (schema: string, table: string) => {
    queryClient.prefetchQuery({
      queryKey: dataBrowserKeys.tableSchema(clusterId, schema, table),
      queryFn: () => dataBrowserApi.getTableSchema(clusterId, schema, table),
      staleTime: 1000 * 60 * 5, // 5 minutes
    });
  };
}
