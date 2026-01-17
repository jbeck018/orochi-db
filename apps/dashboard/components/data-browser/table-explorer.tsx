"use client";

import * as React from "react";
import {
  Table,
  Database,
  Eye,
  Layers,
  Search,
  RefreshCw,
  ChevronRight,
  Key,
  Loader2,
} from "lucide-react";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Badge } from "@/components/ui/badge";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";
import type { TableInfo } from "@/types";

interface TableExplorerProps {
  tables: TableInfo[];
  isLoading: boolean;
  selectedTable: { schema: string; name: string } | null;
  onSelectTable: (schema: string, name: string) => void;
  onRefresh: () => void;
  onPrefetchSchema?: (schema: string, table: string) => void;
}

const tableTypeIcons: Record<string, React.ReactNode> = {
  table: <Table className="h-4 w-4" />,
  view: <Eye className="h-4 w-4" />,
  materialized_view: <Layers className="h-4 w-4" />,
};

const tableTypeLabels: Record<string, string> = {
  table: "Table",
  view: "View",
  materialized_view: "Materialized View",
};

function formatRowCount(count: number): string {
  if (count >= 1_000_000) {
    return `${(count / 1_000_000).toFixed(1)}M`;
  }
  if (count >= 1_000) {
    return `${(count / 1_000).toFixed(1)}K`;
  }
  return count.toString();
}

export function TableExplorer({
  tables,
  isLoading,
  selectedTable,
  onSelectTable,
  onRefresh,
  onPrefetchSchema,
}: TableExplorerProps) {
  const [search, setSearch] = React.useState("");

  // Group tables by schema
  const tablesBySchema = React.useMemo(() => {
    const filtered = tables.filter(
      (t) =>
        t.name.toLowerCase().includes(search.toLowerCase()) ||
        t.schema.toLowerCase().includes(search.toLowerCase())
    );

    return filtered.reduce<Record<string, TableInfo[]>>(
      (acc, table) => {
        const schemaKey = table.schema;
        if (!acc[schemaKey]) {
          acc[schemaKey] = [];
        }
        acc[schemaKey]!.push(table);
        return acc;
      },
      {}
    );
  }, [tables, search]);

  const schemas = Object.keys(tablesBySchema).sort((a, b) => {
    // Put 'public' schema first
    if (a === "public") return -1;
    if (b === "public") return 1;
    return a.localeCompare(b);
  });

  return (
    <div className="flex h-full flex-col">
      {/* Header */}
      <div className="flex items-center justify-between border-b px-4 py-3">
        <div className="flex items-center gap-2">
          <Database className="h-4 w-4 text-muted-foreground" />
          <span className="font-medium">Tables</span>
          <Badge variant="secondary" className="text-xs">
            {tables.length}
          </Badge>
        </div>
        <TooltipProvider>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                variant="ghost"
                size="icon"
                className="h-7 w-7"
                onClick={onRefresh}
                disabled={isLoading}
              >
                <RefreshCw
                  className={cn("h-4 w-4", isLoading && "animate-spin")}
                />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Refresh tables</TooltipContent>
          </Tooltip>
        </TooltipProvider>
      </div>

      {/* Search */}
      <div className="border-b px-4 py-2">
        <div className="relative">
          <Search className="absolute left-2.5 top-2.5 h-4 w-4 text-muted-foreground" />
          <Input
            placeholder="Search tables..."
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            className="h-9 pl-8"
          />
        </div>
      </div>

      {/* Table List */}
      <ScrollArea className="flex-1">
        {isLoading ? (
          <div className="flex items-center justify-center py-8">
            <Loader2 className="h-6 w-6 animate-spin text-muted-foreground" />
          </div>
        ) : schemas.length === 0 ? (
          <div className="flex flex-col items-center justify-center py-8 text-center">
            <Database className="h-8 w-8 text-muted-foreground mb-2" />
            <p className="text-sm text-muted-foreground">
              {search ? "No tables match your search" : "No tables found"}
            </p>
          </div>
        ) : (
          <div className="p-2">
            {schemas.map((schema) => (
              <SchemaGroup
                key={schema}
                schema={schema}
                tables={tablesBySchema[schema] ?? []}
                selectedTable={selectedTable}
                onSelectTable={onSelectTable}
                onPrefetchSchema={onPrefetchSchema}
              />
            ))}
          </div>
        )}
      </ScrollArea>
    </div>
  );
}

interface SchemaGroupProps {
  schema: string;
  tables: TableInfo[];
  selectedTable: { schema: string; name: string } | null;
  onSelectTable: (schema: string, name: string) => void;
  onPrefetchSchema?: (schema: string, table: string) => void;
}

function SchemaGroup({
  schema,
  tables,
  selectedTable,
  onSelectTable,
  onPrefetchSchema,
}: SchemaGroupProps) {
  const [isExpanded, setIsExpanded] = React.useState(schema === "public");

  return (
    <div className="mb-1">
      <button
        onClick={() => setIsExpanded(!isExpanded)}
        className="flex w-full items-center gap-1 rounded-md px-2 py-1.5 text-sm font-medium hover:bg-accent"
      >
        <ChevronRight
          className={cn(
            "h-4 w-4 text-muted-foreground transition-transform",
            isExpanded && "rotate-90"
          )}
        />
        <span>{schema}</span>
        <Badge variant="outline" className="ml-auto text-xs">
          {tables.length}
        </Badge>
      </button>

      {isExpanded && (
        <div className="ml-4 mt-1 space-y-0.5">
          {tables
            .sort((a, b) => a.name.localeCompare(b.name))
            .map((table) => (
              <TableItem
                key={`${table.schema}.${table.name}`}
                table={table}
                isSelected={
                  selectedTable?.schema === table.schema &&
                  selectedTable?.name === table.name
                }
                onSelect={() => onSelectTable(table.schema, table.name)}
                onPrefetch={() => onPrefetchSchema?.(table.schema, table.name)}
              />
            ))}
        </div>
      )}
    </div>
  );
}

interface TableItemProps {
  table: TableInfo;
  isSelected: boolean;
  onSelect: () => void;
  onPrefetch: () => void;
}

const TableItem = React.memo(function TableItem({ table, isSelected, onSelect, onPrefetch }: TableItemProps) {
  return (
    <TooltipProvider>
      <Tooltip>
        <TooltipTrigger asChild>
          <button
            onClick={onSelect}
            onMouseEnter={onPrefetch}
            className={cn(
              "flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-sm",
              isSelected
                ? "bg-primary text-primary-foreground"
                : "hover:bg-accent"
            )}
          >
            <span
              className={cn(
                "text-muted-foreground",
                isSelected && "text-primary-foreground/70"
              )}
            >
              {tableTypeIcons[table.type]}
            </span>
            <span className="flex-1 truncate text-left">{table.name}</span>
            {table.hasPrimaryKey && (
              <Key
                className={cn(
                  "h-3 w-3",
                  isSelected ? "text-primary-foreground/70" : "text-amber-500"
                )}
              />
            )}
            <span
              className={cn(
                "text-xs tabular-nums",
                isSelected ? "text-primary-foreground/70" : "text-muted-foreground"
              )}
            >
              {formatRowCount(table.rowEstimate)}
            </span>
          </button>
        </TooltipTrigger>
        <TooltipContent side="right" className="max-w-xs">
          <div className="space-y-1">
            <p className="font-medium">
              {table.schema}.{table.name}
            </p>
            <p className="text-xs text-muted-foreground">
              {tableTypeLabels[table.type]} &bull; {table.columns} columns &bull;{" "}
              {table.sizeHuman}
            </p>
            <p className="text-xs text-muted-foreground">
              ~{table.rowEstimate.toLocaleString()} rows
            </p>
          </div>
        </TooltipContent>
      </Tooltip>
    </TooltipProvider>
  );
});
