"use client";

import * as React from "react";
import {
  useReactTable,
  getCoreRowModel,
  flexRender,
  type ColumnDef,
} from "@tanstack/react-table";
import {
  ArrowUpDown,
  ArrowUp,
  ArrowDown,
  Key,
  Link2,
  ChevronLeft,
  ChevronRight,
  ChevronsLeft,
  ChevronsRight,
  RefreshCw,
  Copy,
  Check,
} from "lucide-react";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { ScrollArea, ScrollBar } from "@/components/ui/scroll-area";
import { Skeleton } from "@/components/ui/skeleton";
import { cn } from "@/lib/utils";
import type { TableSchema, QueryResult } from "@/types";

interface TableViewerProps {
  schema: TableSchema | undefined;
  data: QueryResult | undefined;
  isLoadingSchema: boolean;
  isLoadingData: boolean;
  page: number;
  pageSize: number;
  sortColumn: string | undefined;
  sortDirection: "asc" | "desc" | undefined;
  onPageChange: (page: number) => void;
  onPageSizeChange: (pageSize: number) => void;
  onSortChange: (column: string, direction: "asc" | "desc") => void;
  onRefresh: () => void;
}

export function TableViewer({
  schema,
  data,
  isLoadingSchema,
  isLoadingData,
  page,
  pageSize,
  sortColumn,
  sortDirection,
  onPageChange,
  onPageSizeChange,
  onSortChange,
  onRefresh,
}: TableViewerProps) {
  const [copiedCell, setCopiedCell] = React.useState<string | null>(null);

  // Build columns from schema
  const columns: ColumnDef<unknown[]>[] = React.useMemo(() => {
    if (!schema) return [];

    return schema.columns.map((col, idx) => ({
      id: col.name,
      accessorFn: (row: unknown[]) => row[idx],
      header: () => {
        const isSorted = sortColumn === col.name;
        const Icon = isSorted
          ? sortDirection === "asc"
            ? ArrowUp
            : ArrowDown
          : ArrowUpDown;

        return (
          <button
            onClick={() => {
              const newDirection =
                sortColumn === col.name && sortDirection === "asc" ? "desc" : "asc";
              onSortChange(col.name, newDirection);
            }}
            className="flex items-center gap-1 font-medium hover:text-foreground"
          >
            <span className="truncate">{col.name}</span>
            {col.isPrimaryKey && (
              <Key className="h-3 w-3 text-amber-500 flex-shrink-0" />
            )}
            {col.isForeignKey && (
              <Link2 className="h-3 w-3 text-blue-500 flex-shrink-0" />
            )}
            <Icon
              className={cn(
                "h-3 w-3 flex-shrink-0",
                isSorted ? "text-primary" : "text-muted-foreground"
              )}
            />
          </button>
        );
      },
      cell: ({ getValue, row }) => {
        const value = getValue();
        const cellId = `${row.index}-${col.name}`;
        const isCopied = copiedCell === cellId;

        return (
          <CellValue
            value={value}
            type={col.type}
            nullable={col.nullable}
            isCopied={isCopied}
            onCopy={() => {
              navigator.clipboard.writeText(formatValue(value));
              setCopiedCell(cellId);
              setTimeout(() => setCopiedCell(null), 2000);
            }}
          />
        );
      },
    }));
  }, [schema, sortColumn, sortDirection, copiedCell, onSortChange]);

  const table = useReactTable({
    data: data?.rows ?? [],
    columns,
    getCoreRowModel: getCoreRowModel(),
    manualSorting: true,
    manualPagination: true,
  });

  if (isLoadingSchema) {
    return <TableViewerSkeleton />;
  }

  if (!schema) {
    return (
      <div className="flex flex-col items-center justify-center h-full text-center py-12">
        <p className="text-muted-foreground">Select a table to view data</p>
      </div>
    );
  }

  const totalPages = data?.totalCount
    ? Math.ceil(data.totalCount / pageSize)
    : 1;

  return (
    <div className="flex h-full flex-col">
      {/* Toolbar */}
      <div className="flex items-center justify-between border-b px-4 py-2">
        <div className="flex items-center gap-2">
          <span className="text-sm font-medium">
            {schema.schema}.{schema.name}
          </span>
          <Badge variant="outline" className="text-xs">
            {schema.columns.length} columns
          </Badge>
          {data && (
            <span className="text-xs text-muted-foreground">
              {data.rowCount.toLocaleString()} rows
              {data.totalCount && ` of ${data.totalCount.toLocaleString()}`}
              {data.truncated && " (truncated)"}
            </span>
          )}
        </div>
        <div className="flex items-center gap-2">
          {data && (
            <span className="text-xs text-muted-foreground">
              {data.executionTimeMs}ms
            </span>
          )}
          <Button
            variant="ghost"
            size="sm"
            onClick={onRefresh}
            disabled={isLoadingData}
          >
            <RefreshCw
              className={cn("h-4 w-4", isLoadingData && "animate-spin")}
            />
          </Button>
        </div>
      </div>

      {/* Table */}
      <ScrollArea className="flex-1">
        <div className="relative min-w-max">
          <Table>
            <TableHeader>
              {table.getHeaderGroups().map((headerGroup) => (
                <TableRow key={headerGroup.id}>
                  {headerGroup.headers.map((header) => (
                    <TableHead
                      key={header.id}
                      className="whitespace-nowrap bg-muted/50 sticky top-0"
                    >
                      {header.isPlaceholder
                        ? null
                        : flexRender(
                            header.column.columnDef.header,
                            header.getContext()
                          )}
                    </TableHead>
                  ))}
                </TableRow>
              ))}
            </TableHeader>
            <TableBody>
              {isLoadingData ? (
                Array.from({ length: pageSize }).map((_, i) => (
                  <TableRow key={i}>
                    {columns.map((_, j) => (
                      <TableCell key={j}>
                        <Skeleton className="h-4 w-24" />
                      </TableCell>
                    ))}
                  </TableRow>
                ))
              ) : table.getRowModel().rows.length === 0 ? (
                <TableRow>
                  <TableCell
                    colSpan={columns.length}
                    className="h-24 text-center"
                  >
                    No data found.
                  </TableCell>
                </TableRow>
              ) : (
                table.getRowModel().rows.map((row) => (
                  <TableRow key={row.id}>
                    {row.getVisibleCells().map((cell) => (
                      <TableCell
                        key={cell.id}
                        className="max-w-xs truncate font-mono text-xs"
                      >
                        {flexRender(
                          cell.column.columnDef.cell,
                          cell.getContext()
                        )}
                      </TableCell>
                    ))}
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
        <ScrollBar orientation="horizontal" />
      </ScrollArea>

      {/* Pagination */}
      <div className="flex items-center justify-between border-t px-4 py-2">
        <div className="flex items-center gap-2">
          <span className="text-sm text-muted-foreground">Rows per page</span>
          <Select
            value={pageSize.toString()}
            onValueChange={(value) => onPageSizeChange(Number(value))}
          >
            <SelectTrigger className="h-8 w-16">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {[25, 50, 100, 250].map((size) => (
                <SelectItem key={size} value={size.toString()}>
                  {size}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        </div>

        <div className="flex items-center gap-2">
          <span className="text-sm text-muted-foreground">
            Page {page} of {totalPages}
          </span>
          <div className="flex items-center gap-1">
            <Button
              variant="outline"
              size="icon"
              className="h-8 w-8"
              onClick={() => onPageChange(1)}
              disabled={page === 1 || isLoadingData}
            >
              <ChevronsLeft className="h-4 w-4" />
            </Button>
            <Button
              variant="outline"
              size="icon"
              className="h-8 w-8"
              onClick={() => onPageChange(page - 1)}
              disabled={page === 1 || isLoadingData}
            >
              <ChevronLeft className="h-4 w-4" />
            </Button>
            <Button
              variant="outline"
              size="icon"
              className="h-8 w-8"
              onClick={() => onPageChange(page + 1)}
              disabled={page >= totalPages || isLoadingData}
            >
              <ChevronRight className="h-4 w-4" />
            </Button>
            <Button
              variant="outline"
              size="icon"
              className="h-8 w-8"
              onClick={() => onPageChange(totalPages)}
              disabled={page >= totalPages || isLoadingData}
            >
              <ChevronsRight className="h-4 w-4" />
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}

interface CellValueProps {
  value: unknown;
  type: string;
  nullable: boolean;
  isCopied: boolean;
  onCopy: () => void;
}

function CellValue({ value, type: _type, nullable, isCopied, onCopy }: CellValueProps) {
  // _type is available for future type-specific formatting
  if (value === null) {
    return (
      <span className="text-muted-foreground italic">
        {nullable ? "NULL" : "null"}
      </span>
    );
  }

  const displayValue = formatValue(value);
  const isLong = displayValue.length > 50;

  return (
    <TooltipProvider>
      <Tooltip>
        <TooltipTrigger asChild>
          <button
            onClick={onCopy}
            className="flex items-center gap-1 hover:bg-accent rounded px-1 -mx-1 text-left"
          >
            <span className="truncate max-w-[200px]">{displayValue}</span>
            {isCopied ? (
              <Check className="h-3 w-3 text-green-500 flex-shrink-0" />
            ) : (
              <Copy className="h-3 w-3 text-muted-foreground opacity-0 group-hover:opacity-100 flex-shrink-0" />
            )}
          </button>
        </TooltipTrigger>
        {isLong && (
          <TooltipContent side="bottom" className="max-w-md">
            <pre className="text-xs whitespace-pre-wrap break-all">
              {displayValue}
            </pre>
          </TooltipContent>
        )}
      </Tooltip>
    </TooltipProvider>
  );
}

function formatValue(value: unknown): string {
  if (value === null || value === undefined) return "NULL";
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

function TableViewerSkeleton() {
  return (
    <div className="flex h-full flex-col">
      <div className="flex items-center justify-between border-b px-4 py-2">
        <div className="flex items-center gap-2">
          <Skeleton className="h-5 w-32" />
          <Skeleton className="h-5 w-16" />
        </div>
        <Skeleton className="h-8 w-8" />
      </div>
      <div className="flex-1 p-4 space-y-2">
        {Array.from({ length: 10 }).map((_, i) => (
          <Skeleton key={i} className="h-8 w-full" />
        ))}
      </div>
    </div>
  );
}
