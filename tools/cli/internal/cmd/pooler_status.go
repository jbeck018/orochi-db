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
	poolerStatusWatch    bool
	poolerStatusInterval time.Duration
)

var poolerStatusCmd = &cobra.Command{
	Use:   "status <cluster-id>",
	Short: "Show pooler status and health",
	Long: `Show the status and health of the PgDog connection pooler.

Displays:
- Overall health status
- Pooler version and uptime
- Current configuration summary
- Connection and query statistics
- Endpoint information

Examples:
  # Show pooler status
  orochi cluster pooler status my-cluster

  # Output as JSON
  orochi cluster pooler status my-cluster --output json

  # Watch status with continuous updates
  orochi cluster pooler status my-cluster --watch

  # Watch with custom interval
  orochi cluster pooler status my-cluster --watch --interval 10s`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerStatus,
}

func init() {
	poolerStatusCmd.Flags().BoolVarP(&poolerStatusWatch, "watch", "w", false, "Continuously watch status updates")
	poolerStatusCmd.Flags().DurationVarP(&poolerStatusInterval, "interval", "i", 2*time.Second, "Refresh interval for watch mode")
}

func runPoolerStatus(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]
	ctx := cmd.Context()

	if poolerStatusWatch {
		return watchPoolerStatus(ctx, clusterID)
	}

	return showPoolerStatus(ctx, clusterID)
}

func showPoolerStatus(ctx context.Context, clusterID string) error {
	client := api.NewClient()
	status, err := client.GetPoolerStatus(ctx, clusterID)
	if err != nil {
		return fmt.Errorf("failed to get pooler status: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, status, func() {
		printPoolerStatus(status)
	})

	return nil
}

func watchPoolerStatus(ctx context.Context, clusterID string) error {
	client := api.NewClient()

	// Clear screen and position cursor
	fmt.Print("\033[2J\033[H")

	ticker := time.NewTicker(poolerStatusInterval)
	defer ticker.Stop()

	// Show initial status
	status, err := client.GetPoolerStatus(ctx, clusterID)
	if err != nil {
		return fmt.Errorf("failed to get pooler status: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(status)
	} else {
		printPoolerStatusCompact(status)
	}

	for {
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
			status, err := client.GetPoolerStatus(ctx, clusterID)
			if err != nil {
				output.Warning("Failed to refresh status: %v", err)
				continue
			}

			// Clear screen and reprint
			fmt.Print("\033[2J\033[H")
			if format == "json" {
				output.PrintJSON(status)
			} else {
				printPoolerStatusCompact(status)
			}
		}
	}
}

func printPoolerStatus(status *api.PoolerStatus) {
	// Header
	output.BoldColor.Printf("Pooler Status: %s\n", status.ClusterName)
	fmt.Println(strings.Repeat("=", 50))
	fmt.Println()

	// Health status
	healthColor := color.New(color.FgGreen)
	healthText := "HEALTHY"
	if !status.Healthy {
		healthColor = color.New(color.FgRed)
		healthText = "UNHEALTHY"
	}
	output.DimColor.Printf("%-18s", "Health:")
	healthColor.Printf(" %s\n", healthText)

	output.DimColor.Printf("%-18s", "Status:")
	fmt.Printf(" %s\n", status.Status)

	output.DimColor.Printf("%-18s", "Version:")
	fmt.Printf(" %s\n", status.Version)

	output.DimColor.Printf("%-18s", "Uptime:")
	fmt.Printf(" %s\n", formatDuration(status.Uptime))

	output.DimColor.Printf("%-18s", "Last Health Check:")
	fmt.Printf(" %s\n", output.FormatTimeRelative(status.LastHealthCheck))

	// Endpoints
	if status.Endpoints != nil {
		fmt.Println()
		output.BoldColor.Println("Endpoints")
		fmt.Println(strings.Repeat("-", 50))

		output.DimColor.Printf("%-18s", "Read/Write:")
		output.InfoColor.Printf(" %s\n", status.Endpoints.ReadWrite)

		if status.Endpoints.ReadOnly != "" {
			output.DimColor.Printf("%-18s", "Read Only:")
			output.InfoColor.Printf(" %s\n", status.Endpoints.ReadOnly)
		}

		if status.Endpoints.Admin != "" {
			output.DimColor.Printf("%-18s", "Admin:")
			output.InfoColor.Printf(" %s\n", status.Endpoints.Admin)
		}
	}

	// Configuration summary
	if status.Config != nil {
		fmt.Println()
		output.BoldColor.Println("Configuration")
		fmt.Println(strings.Repeat("-", 50))

		output.DimColor.Printf("%-18s", "Mode:")
		fmt.Printf(" %s\n", status.Config.Mode)

		output.DimColor.Printf("%-18s", "Pool Size:")
		fmt.Printf(" %d - %d\n", status.Config.MinPoolSize, status.Config.MaxPoolSize)

		output.DimColor.Printf("%-18s", "Idle Timeout:")
		fmt.Printf(" %ds\n", status.Config.IdleTimeout)

		output.DimColor.Printf("%-18s", "Read/Write Split:")
		fmt.Printf(" %s\n", formatBool(status.Config.ReadWriteSplit))

		output.DimColor.Printf("%-18s", "Sharding:")
		if status.Config.ShardingEnabled {
			fmt.Printf(" Enabled (%d shards)\n", status.Config.ShardCount)
		} else {
			fmt.Printf(" Disabled\n")
		}

		output.DimColor.Printf("%-18s", "Load Balancing:")
		fmt.Printf(" %s\n", status.Config.LoadBalancing)
	}

	// Quick stats
	if status.Stats != nil {
		fmt.Println()
		output.BoldColor.Println("Quick Statistics")
		fmt.Println(strings.Repeat("-", 50))

		output.DimColor.Printf("%-18s", "Connections:")
		fmt.Printf(" %d active, %d idle, %d total\n",
			status.Stats.ActiveConnections,
			status.Stats.IdleConnections,
			status.Stats.TotalConnections)

		output.DimColor.Printf("%-18s", "Queries:")
		fmt.Printf(" %.1f/s (total: %d)\n",
			status.Stats.QueriesPerSecond,
			status.Stats.TotalQueries)

		output.DimColor.Printf("%-18s", "Latency (avg):")
		fmt.Printf(" %.2fms\n", status.Stats.AvgLatencyMs)

		output.DimColor.Printf("%-18s", "Pool Utilization:")
		utilizationColor := getUtilizationColor(status.Stats.PoolUtilization)
		utilizationColor.Printf(" %.1f%%\n", status.Stats.PoolUtilization)

		if status.Stats.TotalErrors > 0 {
			output.DimColor.Printf("%-18s", "Errors:")
			output.ErrorColor.Printf(" %d (%.2f%%)\n",
				status.Stats.TotalErrors,
				status.Stats.ErrorRate)
		}
	}

	fmt.Println()
	output.Dim("Use 'orochi cluster pooler stats %s' for detailed statistics", status.ClusterName)
}

func printPoolerStatusCompact(status *api.PoolerStatus) {
	// Compact format for watch mode
	now := time.Now().Format("15:04:05")

	// Header with timestamp
	output.BoldColor.Printf("Pooler: %s", status.ClusterName)
	output.DimColor.Printf(" (updated %s)\n", now)
	fmt.Println(strings.Repeat("-", 60))

	// Health
	healthColor := color.New(color.FgGreen)
	healthIcon := "[OK]"
	if !status.Healthy {
		healthColor = color.New(color.FgRed)
		healthIcon = "[!!]"
	}
	healthColor.Printf("%s ", healthIcon)
	fmt.Printf("Status: %s | Version: %s | Uptime: %s\n",
		status.Status, status.Version, formatDuration(status.Uptime))

	// Stats line
	if status.Stats != nil {
		fmt.Printf("    Connections: %d/%d | Queries: %.1f/s | Latency: %.2fms | Util: %.1f%%\n",
			status.Stats.ActiveConnections,
			status.Stats.TotalConnections,
			status.Stats.QueriesPerSecond,
			status.Stats.AvgLatencyMs,
			status.Stats.PoolUtilization)

		if status.Stats.TotalErrors > 0 {
			output.ErrorColor.Printf("    Errors: %d | Error Rate: %.2f%%\n",
				status.Stats.TotalErrors,
				status.Stats.ErrorRate)
		}
	}

	fmt.Println()
	output.Dim("Press Ctrl+C to stop watching")
}

func formatDuration(d time.Duration) string {
	if d < time.Minute {
		return fmt.Sprintf("%ds", int(d.Seconds()))
	}
	if d < time.Hour {
		return fmt.Sprintf("%dm %ds", int(d.Minutes()), int(d.Seconds())%60)
	}
	if d < 24*time.Hour {
		hours := int(d.Hours())
		minutes := int(d.Minutes()) % 60
		return fmt.Sprintf("%dh %dm", hours, minutes)
	}
	days := int(d.Hours()) / 24
	hours := int(d.Hours()) % 24
	return fmt.Sprintf("%dd %dh", days, hours)
}

func formatBool(b bool) string {
	if b {
		return "Enabled"
	}
	return "Disabled"
}

func getUtilizationColor(utilization float64) *color.Color {
	switch {
	case utilization >= 90:
		return color.New(color.FgRed, color.Bold)
	case utilization >= 70:
		return color.New(color.FgYellow)
	default:
		return color.New(color.FgGreen)
	}
}
