"use client";

import * as React from "react";
import {
  Database,
  Terminal,
  BarChart3,
  PanelLeftClose,
  PanelLeft,
} from "lucide-react";
import { Tabs, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Button } from "@/components/ui/button";
import {
  ResizableHandle,
  ResizablePanel,
  ResizablePanelGroup,
} from "@/components/ui/resizable";
import { cn } from "@/lib/utils";
import {
  useTables,
  useTableSchema,
  useTableData,
  useQueryHistory,
  useInternalStats,
  useExecuteSQL,
  usePrefetchTableSchema,
} from "@/hooks/api";
import { TableExplorer } from "./table-explorer";
import { TableViewer } from "./table-viewer";
import { SQLEditor } from "./sql-editor";
import { InternalStats } from "./internal-stats";
import type { DataBrowserTableParams } from "@/lib/api";

interface DataBrowserProps {
  clusterId: string;
}

export function DataBrowser({ clusterId }: DataBrowserProps) {
  const [selectedTable, setSelectedTable] = React.useState<{
    schema: string;
    name: string;
  } | null>(null);
  const [sidebarCollapsed, setSidebarCollapsed] = React.useState(false);
  const [activeTab, setActiveTab] = React.useState<"data" | "sql" | "stats">("data");

  // Table data pagination and sorting state
  const [page, setPage] = React.useState(1);
  const [pageSize, setPageSize] = React.useState(50);
  const [sortColumn, setSortColumn] = React.useState<string | undefined>();
  const [sortDirection, setSortDirection] = React.useState<"asc" | "desc" | undefined>();

  // Reset pagination when table changes
  React.useEffect(() => {
    setPage(1);
    setSortColumn(undefined);
    setSortDirection(undefined);
  }, [selectedTable?.schema, selectedTable?.name]);

  // Queries
  const {
    data: tables = [],
    isLoading: tablesLoading,
    refetch: refetchTables,
  } = useTables(clusterId);

  const {
    data: tableSchema,
    isLoading: schemaLoading,
  } = useTableSchema(
    clusterId,
    selectedTable?.schema ?? "",
    selectedTable?.name ?? "",
    !!selectedTable
  );

  const tableParams: DataBrowserTableParams = React.useMemo(
    () => ({
      page,
      pageSize,
      sortColumn,
      sortDirection,
    }),
    [page, pageSize, sortColumn, sortDirection]
  );

  const {
    data: tableData,
    isLoading: dataLoading,
    refetch: refetchData,
  } = useTableData(
    clusterId,
    selectedTable?.schema ?? "",
    selectedTable?.name ?? "",
    tableParams,
    !!selectedTable
  );

  const {
    data: queryHistory = [],
    isLoading: historyLoading,
  } = useQueryHistory(clusterId, 50, activeTab === "sql");

  const {
    data: internalStats,
    isLoading: statsLoading,
    refetch: refetchStats,
  } = useInternalStats(clusterId, activeTab === "stats");

  const executeSQL = useExecuteSQL(clusterId);
  const prefetchSchema = usePrefetchTableSchema(clusterId);

  const handleSelectTable = React.useCallback((schema: string, name: string) => {
    setSelectedTable({ schema, name });
    setActiveTab("data");
  }, []);

  const handleSortChange = React.useCallback((column: string, direction: "asc" | "desc") => {
    setSortColumn(column);
    setSortDirection(direction);
    setPage(1); // Reset to first page on sort change
  }, []);

  const handlePageSizeChange = React.useCallback((newPageSize: number) => {
    setPageSize(newPageSize);
    setPage(1); // Reset to first page on page size change
  }, []);

  const handleExecuteSQL = React.useCallback(async (sql: string, readOnly = true) => {
    const result = await executeSQL.mutateAsync({ sql, readOnly });
    return result;
  }, [executeSQL]);

  return (
    <div className="h-full flex flex-col">
      {/* Tab Navigation */}
      <div className="border-b px-4">
        <div className="flex items-center justify-between">
          <Tabs value={activeTab} onValueChange={(v) => setActiveTab(v as typeof activeTab)}>
            <TabsList className="h-10">
              <TabsTrigger value="data" className="gap-2">
                <Database className="h-4 w-4" />
                Tables
              </TabsTrigger>
              <TabsTrigger value="sql" className="gap-2">
                <Terminal className="h-4 w-4" />
                SQL Editor
              </TabsTrigger>
              <TabsTrigger value="stats" className="gap-2">
                <BarChart3 className="h-4 w-4" />
                Statistics
              </TabsTrigger>
            </TabsList>
          </Tabs>

          <Button
            variant="ghost"
            size="sm"
            onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
            className={cn("gap-2", activeTab !== "data" && "hidden")}
          >
            {sidebarCollapsed ? (
              <>
                <PanelLeft className="h-4 w-4" />
                Show Tables
              </>
            ) : (
              <>
                <PanelLeftClose className="h-4 w-4" />
                Hide Tables
              </>
            )}
          </Button>
        </div>
      </div>

      {/* Content */}
      <div className="flex-1 overflow-hidden">
        {activeTab === "data" && (
          <ResizablePanelGroup orientation="horizontal" className="h-full">
            {!sidebarCollapsed && (
              <>
                <ResizablePanel
                  defaultSize={25}
                  minSize={15}
                  maxSize={40}
                  className="bg-muted/30"
                >
                  <TableExplorer
                    tables={tables}
                    isLoading={tablesLoading}
                    selectedTable={selectedTable}
                    onSelectTable={handleSelectTable}
                    onRefresh={() => refetchTables()}
                    onPrefetchSchema={prefetchSchema}
                  />
                </ResizablePanel>
                <ResizableHandle withHandle />
              </>
            )}
            <ResizablePanel defaultSize={sidebarCollapsed ? 100 : 75}>
              <TableViewer
                schema={tableSchema}
                data={tableData}
                isLoadingSchema={schemaLoading}
                isLoadingData={dataLoading}
                page={page}
                pageSize={pageSize}
                sortColumn={sortColumn}
                sortDirection={sortDirection}
                onPageChange={setPage}
                onPageSizeChange={handlePageSizeChange}
                onSortChange={handleSortChange}
                onRefresh={() => refetchData()}
              />
            </ResizablePanel>
          </ResizablePanelGroup>
        )}

        {activeTab === "sql" && (
          <SQLEditor
            onExecute={handleExecuteSQL}
            isExecuting={executeSQL.isPending}
            history={queryHistory}
            isLoadingHistory={historyLoading}
          />
        )}

        {activeTab === "stats" && (
          <div className="p-4 overflow-auto h-full">
            <InternalStats
              stats={internalStats}
              isLoading={statsLoading}
              onRefresh={() => refetchStats()}
            />
          </div>
        )}
      </div>
    </div>
  );
}
