package cmd

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/fatih/color"
	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	statsInterval time.Duration
	statsFormat   string
)

var poolerStatsCmd = &cobra.Command{
	Use:   "stats <cluster-id>",
	Short: "View detailed pooler statistics",
	Long: `View detailed statistics for the PgDog connection pooler.

Displays comprehensive metrics including:
- Connection statistics (active, idle, waiting, total)
- Query statistics (total, per second, read/write breakdown)
- Latency metrics (average, P50, P95, P99, max)
- Traffic statistics (bytes sent/received)
- Error statistics (total, by type, error rate)
- Transaction statistics (total, commits, rollbacks)
- Pool utilization percentage

Output Formats:
  table  - Human-readable table format (default)
  json   - JSON output for scripting
  wide   - Extended table with more columns

Examples:
  # Show pooler statistics
  orochi cluster pooler stats my-cluster

  # Show statistics in JSON format
  orochi cluster pooler stats my-cluster --output json

  # Show extended statistics
  orochi cluster pooler stats my-cluster --format wide

  # Continuously poll statistics every 5 seconds
  orochi cluster pooler stats my-cluster --interval 5s

  # Poll statistics every 10 seconds in JSON format
  orochi cluster pooler stats my-cluster --interval 10s --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerStats,
}

func init() {
	poolerStatsCmd.Flags().DurationVarP(&statsInterval, "interval", "i", 0, "Polling interval for continuous stats (e.g., 5s, 10s)")
	poolerStatsCmd.Flags().StringVarP(&statsFormat, "format", "f", "table", "Output format (table, wide)")
}

func runPoolerStats(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]
	ctx := cmd.Context()

	if statsInterval > 0 {
		return pollPoolerStats(ctx, clusterID)
	}

	return showPoolerStats(ctx, clusterID)
}

func showPoolerStats(ctx context.Context, clusterID string) error {
	client := api.NewClient()
	stats, err := client.GetPoolerStats(ctx, clusterID)
	if err != nil {
		return fmt.Errorf("failed to get pooler stats: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, stats, func() {
		if statsFormat == "wide" {
			printPoolerStatsWide(clusterID, stats)
		} else {
			printPoolerStats(clusterID, stats)
		}
	})

	return nil
}

func pollPoolerStats(ctx context.Context, clusterID string) error {
	client := api.NewClient()

	ticker := time.NewTicker(statsInterval)
	defer ticker.Stop()

	// Show initial stats
	stats, err := client.GetPoolerStats(ctx, clusterID)
	if err != nil {
		return fmt.Errorf("failed to get pooler stats: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(stats)
	} else {
		// Clear screen for first display
		fmt.Print("\033[2J\033[H")
		printPoolerStatsCompact(clusterID, stats)
	}

	for {
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
			stats, err := client.GetPoolerStats(ctx, clusterID)
			if err != nil {
				output.Warning("Failed to refresh stats: %v", err)
				continue
			}

			if format == "json" {
				output.PrintJSON(stats)
			} else {
				fmt.Print("\033[2J\033[H")
				printPoolerStatsCompact(clusterID, stats)
			}
		}
	}
}

func printPoolerStats(clusterID string, stats *api.PoolerStats) {
	output.BoldColor.Printf("Pooler Statistics: %s\n", clusterID)
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("Timestamp: %s\n", stats.Timestamp.Format(time.RFC3339))
	fmt.Println()

	// Connection Statistics
	output.BoldColor.Println("Connection Statistics")
	fmt.Println(strings.Repeat("-", 60))

	table := output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Active Connections", fmt.Sprintf("%d", stats.ActiveConnections)})
	table.Append([]string{"Idle Connections", fmt.Sprintf("%d", stats.IdleConnections)})
	table.Append([]string{"Total Connections", fmt.Sprintf("%d", stats.TotalConnections)})
	table.Append([]string{"Waiting Clients", fmt.Sprintf("%d", stats.WaitingClients)})
	table.Append([]string{"Max Connections Used", fmt.Sprintf("%d", stats.MaxConnectionsUsed)})
	table.Append([]string{"Connections Created", fmt.Sprintf("%d", stats.ConnectionsCreated)})
	table.Append([]string{"Connections Closed", fmt.Sprintf("%d", stats.ConnectionsClosed)})
	table.Append([]string{"Connections Recycled", fmt.Sprintf("%d", stats.ConnectionsRecycled)})
	table.Append([]string{"Pool Utilization", formatUtilization(stats.PoolUtilization)})
	table.Render()

	fmt.Println()

	// Query Statistics
	output.BoldColor.Println("Query Statistics")
	fmt.Println(strings.Repeat("-", 60))

	table = output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Total Queries", formatLargeNumber(stats.TotalQueries)})
	table.Append([]string{"Queries/Second", fmt.Sprintf("%.2f", stats.QueriesPerSecond)})
	table.Append([]string{"Read Queries", formatLargeNumber(stats.ReadQueries)})
	table.Append([]string{"Write Queries", formatLargeNumber(stats.WriteQueries)})
	table.Render()

	fmt.Println()

	// Latency Statistics
	output.BoldColor.Println("Latency Statistics")
	fmt.Println(strings.Repeat("-", 60))

	table = output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Average Latency", fmt.Sprintf("%.2f ms", stats.AvgLatencyMs)})
	table.Append([]string{"P50 Latency", fmt.Sprintf("%.2f ms", stats.P50LatencyMs)})
	table.Append([]string{"P95 Latency", fmt.Sprintf("%.2f ms", stats.P95LatencyMs)})
	table.Append([]string{"P99 Latency", fmt.Sprintf("%.2f ms", stats.P99LatencyMs)})
	table.Append([]string{"Max Latency", fmt.Sprintf("%.2f ms", stats.MaxLatencyMs)})
	table.Append([]string{"Total Wait Time", fmt.Sprintf("%.2f ms", stats.TotalWaitMs)})
	table.Append([]string{"Average Wait Time", fmt.Sprintf("%.2f ms", stats.AvgWaitMs)})
	table.Render()

	fmt.Println()

	// Traffic Statistics
	output.BoldColor.Println("Traffic Statistics")
	fmt.Println(strings.Repeat("-", 60))

	table = output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Bytes Sent", formatBytes(stats.BytesSent)})
	table.Append([]string{"Bytes Received", formatBytes(stats.BytesReceived)})
	table.Render()

	fmt.Println()

	// Error Statistics
	output.BoldColor.Println("Error Statistics")
	fmt.Println(strings.Repeat("-", 60))

	errorColor := color.New(color.FgWhite)
	if stats.TotalErrors > 0 {
		errorColor = color.New(color.FgRed)
	}

	table = output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Total Errors", errorColor.Sprintf("%d", stats.TotalErrors)})
	table.Append([]string{"Connection Errors", fmt.Sprintf("%d", stats.ConnectionErrors)})
	table.Append([]string{"Query Errors", fmt.Sprintf("%d", stats.QueryErrors)})
	table.Append([]string{"Timeout Errors", fmt.Sprintf("%d", stats.TimeoutErrors)})
	table.Append([]string{"Error Rate", fmt.Sprintf("%.4f%%", stats.ErrorRate)})
	table.Append([]string{"Banned Connections", fmt.Sprintf("%d", stats.BannedConnections)})
	table.Render()

	fmt.Println()

	// Transaction Statistics
	output.BoldColor.Println("Transaction Statistics")
	fmt.Println(strings.Repeat("-", 60))

	table = output.NewTable([]string{"METRIC", "VALUE"})
	table.Append([]string{"Total Transactions", formatLargeNumber(stats.TransactionsTotal)})
	table.Append([]string{"Commits", formatLargeNumber(stats.TransactionsCommit)})
	table.Append([]string{"Rollbacks", formatLargeNumber(stats.TransactionsRollback)})
	table.Render()
}

func printPoolerStatsWide(clusterID string, stats *api.PoolerStats) {
	output.BoldColor.Printf("Pooler Statistics (Extended): %s\n", clusterID)
	fmt.Println(strings.Repeat("=", 80))
	fmt.Printf("Timestamp: %s\n\n", stats.Timestamp.Format(time.RFC3339))

	// All stats in one comprehensive table
	table := output.NewTable([]string{"CATEGORY", "METRIC", "VALUE", "ADDITIONAL INFO"})

	// Connections
	table.Append([]string{"Connections", "Active", fmt.Sprintf("%d", stats.ActiveConnections), ""})
	table.Append([]string{"", "Idle", fmt.Sprintf("%d", stats.IdleConnections), ""})
	table.Append([]string{"", "Total", fmt.Sprintf("%d", stats.TotalConnections), fmt.Sprintf("Max Used: %d", stats.MaxConnectionsUsed)})
	table.Append([]string{"", "Waiting", fmt.Sprintf("%d", stats.WaitingClients), ""})
	table.Append([]string{"", "Created/Closed", fmt.Sprintf("%d/%d", stats.ConnectionsCreated, stats.ConnectionsClosed), fmt.Sprintf("Recycled: %d", stats.ConnectionsRecycled)})
	table.Append([]string{"", "Utilization", formatUtilization(stats.PoolUtilization), ""})

	// Queries
	table.Append([]string{"Queries", "Total", formatLargeNumber(stats.TotalQueries), fmt.Sprintf("%.2f/s", stats.QueriesPerSecond)})
	table.Append([]string{"", "Read/Write", fmt.Sprintf("%s / %s", formatLargeNumber(stats.ReadQueries), formatLargeNumber(stats.WriteQueries)), ""})

	// Latency
	table.Append([]string{"Latency", "Average", fmt.Sprintf("%.2f ms", stats.AvgLatencyMs), ""})
	table.Append([]string{"", "Percentiles", fmt.Sprintf("P50: %.2f ms", stats.P50LatencyMs), fmt.Sprintf("P95: %.2f, P99: %.2f ms", stats.P95LatencyMs, stats.P99LatencyMs)})
	table.Append([]string{"", "Max", fmt.Sprintf("%.2f ms", stats.MaxLatencyMs), ""})
	table.Append([]string{"", "Wait (Avg/Total)", fmt.Sprintf("%.2f / %.2f ms", stats.AvgWaitMs, stats.TotalWaitMs), ""})

	// Traffic
	table.Append([]string{"Traffic", "Sent/Received", fmt.Sprintf("%s / %s", formatBytes(stats.BytesSent), formatBytes(stats.BytesReceived)), ""})

	// Errors
	errorStr := fmt.Sprintf("%d", stats.TotalErrors)
	if stats.TotalErrors > 0 {
		errorStr = color.New(color.FgRed).Sprint(errorStr)
	}
	table.Append([]string{"Errors", "Total", errorStr, fmt.Sprintf("Rate: %.4f%%", stats.ErrorRate)})
	table.Append([]string{"", "By Type", fmt.Sprintf("Conn: %d, Query: %d, Timeout: %d", stats.ConnectionErrors, stats.QueryErrors, stats.TimeoutErrors), fmt.Sprintf("Banned: %d", stats.BannedConnections)})

	// Transactions
	table.Append([]string{"Transactions", "Total", formatLargeNumber(stats.TransactionsTotal), ""})
	table.Append([]string{"", "Commit/Rollback", fmt.Sprintf("%s / %s", formatLargeNumber(stats.TransactionsCommit), formatLargeNumber(stats.TransactionsRollback)), ""})

	table.Render()
}

func printPoolerStatsCompact(clusterID string, stats *api.PoolerStats) {
	now := time.Now().Format("15:04:05")

	output.BoldColor.Printf("Pooler Stats: %s", clusterID)
	output.DimColor.Printf(" (updated %s, polling every %s)\n", now, statsInterval)
	fmt.Println(strings.Repeat("=", 70))

	// Quick stats bar
	utilizationColor := getUtilizationColor(stats.PoolUtilization)

	fmt.Printf("Connections: ")
	color.New(color.FgGreen).Printf("%d active", stats.ActiveConnections)
	fmt.Printf(" | ")
	color.New(color.FgYellow).Printf("%d idle", stats.IdleConnections)
	fmt.Printf(" | ")
	fmt.Printf("%d total", stats.TotalConnections)
	fmt.Printf(" | Util: ")
	utilizationColor.Printf("%.1f%%", stats.PoolUtilization)
	fmt.Println()

	fmt.Printf("Queries:     ")
	fmt.Printf("%.2f/s", stats.QueriesPerSecond)
	fmt.Printf(" | Total: %s", formatLargeNumber(stats.TotalQueries))
	fmt.Printf(" | R/W: %s/%s", formatLargeNumber(stats.ReadQueries), formatLargeNumber(stats.WriteQueries))
	fmt.Println()

	fmt.Printf("Latency:     ")
	fmt.Printf("Avg: %.2fms", stats.AvgLatencyMs)
	fmt.Printf(" | P95: %.2fms", stats.P95LatencyMs)
	fmt.Printf(" | P99: %.2fms", stats.P99LatencyMs)
	fmt.Printf(" | Max: %.2fms", stats.MaxLatencyMs)
	fmt.Println()

	fmt.Printf("Traffic:     ")
	fmt.Printf("Sent: %s", formatBytes(stats.BytesSent))
	fmt.Printf(" | Recv: %s", formatBytes(stats.BytesReceived))
	fmt.Println()

	if stats.TotalErrors > 0 {
		fmt.Printf("Errors:      ")
		color.New(color.FgRed).Printf("%d total (%.4f%%)", stats.TotalErrors, stats.ErrorRate)
		fmt.Printf(" | Conn: %d | Query: %d | Timeout: %d",
			stats.ConnectionErrors, stats.QueryErrors, stats.TimeoutErrors)
		fmt.Println()
	}

	fmt.Printf("Transactions: %s total | %s commit | %s rollback\n",
		formatLargeNumber(stats.TransactionsTotal),
		formatLargeNumber(stats.TransactionsCommit),
		formatLargeNumber(stats.TransactionsRollback))

	fmt.Println()
	output.Dim("Press Ctrl+C to stop polling")
}

func formatUtilization(util float64) string {
	color := getUtilizationColor(util)
	return color.Sprintf("%.1f%%", util)
}

func formatLargeNumber(n int64) string {
	if n < 1000 {
		return fmt.Sprintf("%d", n)
	}
	if n < 1000000 {
		return fmt.Sprintf("%.1fK", float64(n)/1000)
	}
	if n < 1000000000 {
		return fmt.Sprintf("%.1fM", float64(n)/1000000)
	}
	return fmt.Sprintf("%.1fB", float64(n)/1000000000)
}

func formatBytes(bytes int64) string {
	const (
		KB = 1024
		MB = KB * 1024
		GB = MB * 1024
		TB = GB * 1024
	)

	switch {
	case bytes >= TB:
		return fmt.Sprintf("%.2f TB", float64(bytes)/TB)
	case bytes >= GB:
		return fmt.Sprintf("%.2f GB", float64(bytes)/GB)
	case bytes >= MB:
		return fmt.Sprintf("%.2f MB", float64(bytes)/MB)
	case bytes >= KB:
		return fmt.Sprintf("%.2f KB", float64(bytes)/KB)
	default:
		return fmt.Sprintf("%d B", bytes)
	}
}
