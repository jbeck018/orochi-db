"use client";

import * as React from "react";
import {
  Play,
  Loader2,
  History,
  AlertCircle,
  ChevronDown,
  ChevronUp,
  Copy,
  Check,
  Clock,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { ScrollArea, ScrollBar } from "@/components/ui/scroll-area";
import {
  Collapsible,
  CollapsibleContent,
} from "@/components/ui/collapsible";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import { Skeleton } from "@/components/ui/skeleton";
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";
import type { QueryResult, QueryHistoryEntry } from "@/types";

interface SQLEditorProps {
  onExecute: (sql: string, readOnly?: boolean) => Promise<QueryResult>;
  isExecuting: boolean;
  history: QueryHistoryEntry[];
  isLoadingHistory: boolean;
}

export function SQLEditor({
  onExecute,
  isExecuting,
  history,
  isLoadingHistory,
}: SQLEditorProps) {
  const [sql, setSql] = React.useState("");
  const [result, setResult] = React.useState<QueryResult | null>(null);
  const [error, setError] = React.useState<string | null>(null);
  const [showHistory, setShowHistory] = React.useState(false);
  const textareaRef = React.useRef<HTMLTextAreaElement>(null);

  const handleExecute = async () => {
    if (!sql.trim() || isExecuting) return;

    setError(null);
    setResult(null);

    try {
      // Determine if it's a read-only query
      const trimmed = sql.trim().toLowerCase();
      const readOnly = trimmed.startsWith("select") || trimmed.startsWith("explain");

      const queryResult = await onExecute(sql, readOnly);
      setResult(queryResult);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Query execution failed");
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    // Cmd/Ctrl + Enter to execute
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      handleExecute();
    }
  };

  const loadFromHistory = (entry: QueryHistoryEntry) => {
    setSql(entry.queryText);
    setShowHistory(false);
    textareaRef.current?.focus();
  };

  return (
    <div className="flex h-full flex-col">
      {/* Editor */}
      <div className="border-b">
        <div className="flex items-center justify-between border-b px-4 py-2 bg-muted/30">
          <span className="text-sm font-medium">SQL Editor</span>
          <div className="flex items-center gap-2">
            <TooltipProvider>
              <Tooltip>
                <TooltipTrigger asChild>
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={() => setShowHistory(!showHistory)}
                    className="gap-1"
                  >
                    <History className="h-4 w-4" />
                    History
                    {showHistory ? (
                      <ChevronUp className="h-3 w-3" />
                    ) : (
                      <ChevronDown className="h-3 w-3" />
                    )}
                  </Button>
                </TooltipTrigger>
                <TooltipContent>View query history</TooltipContent>
              </Tooltip>
            </TooltipProvider>
          </div>
        </div>

        {/* History Panel */}
        <Collapsible open={showHistory} onOpenChange={setShowHistory}>
          <CollapsibleContent>
            <div className="border-b bg-muted/20">
              <ScrollArea className="h-48">
                {isLoadingHistory ? (
                  <div className="p-4 space-y-2">
                    {Array.from({ length: 5 }).map((_, i) => (
                      <Skeleton key={i} className="h-12 w-full" />
                    ))}
                  </div>
                ) : history.length === 0 ? (
                  <div className="flex flex-col items-center justify-center py-8 text-center">
                    <History className="h-8 w-8 text-muted-foreground mb-2" />
                    <p className="text-sm text-muted-foreground">
                      No query history yet
                    </p>
                  </div>
                ) : (
                  <div className="p-2 space-y-1">
                    {history.map((entry) => (
                      <HistoryItem
                        key={entry.id}
                        entry={entry}
                        onSelect={() => loadFromHistory(entry)}
                      />
                    ))}
                  </div>
                )}
              </ScrollArea>
            </div>
          </CollapsibleContent>
        </Collapsible>

        {/* SQL Input */}
        <div className="relative">
          <textarea
            ref={textareaRef}
            value={sql}
            onChange={(e) => setSql(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="SELECT * FROM your_table LIMIT 100;"
            className={cn(
              "w-full min-h-[120px] resize-none bg-background p-4 font-mono text-sm",
              "focus:outline-none focus:ring-0 border-0",
              "placeholder:text-muted-foreground"
            )}
          />
          <div className="absolute bottom-2 right-2 flex items-center gap-2">
            <span className="text-xs text-muted-foreground">
              {navigator.platform.includes("Mac") ? "⌘" : "Ctrl"} + Enter to run
            </span>
            <Button
              size="sm"
              onClick={handleExecute}
              disabled={!sql.trim() || isExecuting}
            >
              {isExecuting ? (
                <Loader2 className="h-4 w-4 animate-spin" />
              ) : (
                <Play className="h-4 w-4" />
              )}
              <span className="ml-1">Run</span>
            </Button>
          </div>
        </div>
      </div>

      {/* Results */}
      <div className="flex-1 overflow-hidden">
        {error && (
          <Alert variant="destructive" className="m-4">
            <AlertCircle className="h-4 w-4" />
            <AlertDescription>{error}</AlertDescription>
          </Alert>
        )}

        {result && <QueryResultView result={result} />}

        {!error && !result && (
          <div className="flex flex-col items-center justify-center h-full text-center py-12">
            <p className="text-muted-foreground">
              Write a SQL query and click Run to see results
            </p>
          </div>
        )}
      </div>
    </div>
  );
}

interface HistoryItemProps {
  entry: QueryHistoryEntry;
  onSelect: () => void;
}

const HistoryItem = React.memo(function HistoryItem({ entry, onSelect }: HistoryItemProps) {
  const [copied, setCopied] = React.useState(false);

  const handleCopy = (e: React.MouseEvent) => {
    e.stopPropagation();
    navigator.clipboard.writeText(entry.queryText);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <button
      onClick={onSelect}
      className="w-full text-left p-2 rounded-md hover:bg-accent group"
    >
      <div className="flex items-start justify-between gap-2">
        <pre className="text-xs font-mono truncate flex-1 text-muted-foreground group-hover:text-foreground">
          {entry.queryText.slice(0, 100)}
          {entry.queryText.length > 100 && "..."}
        </pre>
        <div className="flex items-center gap-1 flex-shrink-0">
          <Button
            variant="ghost"
            size="icon"
            className="h-6 w-6 opacity-0 group-hover:opacity-100"
            onClick={handleCopy}
          >
            {copied ? (
              <Check className="h-3 w-3 text-green-500" />
            ) : (
              <Copy className="h-3 w-3" />
            )}
          </Button>
          <Badge
            variant={entry.status === "success" ? "secondary" : "destructive"}
            className="text-xs"
          >
            {entry.status}
          </Badge>
        </div>
      </div>
      <div className="flex items-center gap-2 mt-1 text-xs text-muted-foreground">
        <Clock className="h-3 w-3" />
        <span>{new Date(entry.createdAt).toLocaleString()}</span>
        <span>&bull;</span>
        <span>{entry.executionTimeMs}ms</span>
        {entry.rowsAffected > 0 && (
          <>
            <span>&bull;</span>
            <span>{entry.rowsAffected} rows</span>
          </>
        )}
      </div>
    </button>
  );
});

interface QueryResultViewProps {
  result: QueryResult;
}

function QueryResultView({ result }: QueryResultViewProps) {
  return (
    <div className="flex flex-col h-full">
      {/* Result Header */}
      <div className="flex items-center justify-between px-4 py-2 border-b bg-muted/30">
        <div className="flex items-center gap-2">
          <Badge variant="secondary" className="text-xs">
            {result.rowCount} rows
          </Badge>
          {result.truncated && (
            <Badge variant="outline" className="text-xs text-amber-600">
              Truncated
            </Badge>
          )}
        </div>
        <span className="text-xs text-muted-foreground">
          Executed in {result.executionTimeMs}ms
        </span>
      </div>

      {/* Result Table */}
      <ScrollArea className="flex-1">
        <div className="relative min-w-max">
          <Table>
            <TableHeader>
              <TableRow>
                {result.columns.map((col, idx) => (
                  <TableHead
                    key={col}
                    className="whitespace-nowrap bg-muted/50 sticky top-0"
                  >
                    <div className="flex flex-col">
                      <span>{col}</span>
                      <span className="text-xs font-normal text-muted-foreground">
                        {result.columnTypes[idx]}
                      </span>
                    </div>
                  </TableHead>
                ))}
              </TableRow>
            </TableHeader>
            <TableBody>
              {result.rows.length === 0 ? (
                <TableRow>
                  <TableCell
                    colSpan={result.columns.length}
                    className="h-24 text-center text-muted-foreground"
                  >
                    Query returned no results
                  </TableCell>
                </TableRow>
              ) : (
                result.rows.map((row, rowIdx) => (
                  <TableRow key={rowIdx}>
                    {row.map((cell, cellIdx) => (
                      <TableCell
                        key={cellIdx}
                        className="max-w-xs truncate font-mono text-xs"
                      >
                        <ResultCell value={cell} />
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
    </div>
  );
}

function ResultCell({ value }: { value: unknown }) {
  if (value === null) {
    return <span className="text-muted-foreground italic">NULL</span>;
  }

  const displayValue =
    typeof value === "object" ? JSON.stringify(value) : String(value);

  return (
    <TooltipProvider>
      <Tooltip>
        <TooltipTrigger asChild>
          <span className="truncate max-w-[200px] block">{displayValue}</span>
        </TooltipTrigger>
        {displayValue.length > 50 && (
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
