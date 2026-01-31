package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	metricsCluster   string
	metricsPeriod    string
	metricsInterval  string
	metricsWatchMode bool
)

var metricsCmd = &cobra.Command{
	Use:   "metrics",
	Short: "View cluster metrics and performance data",
	Long: `View metrics and performance data for your clusters.

Commands:
  summary     Show metrics summary for a cluster
  cpu         Show CPU metrics
  memory      Show memory metrics
  storage     Show storage metrics
  queries     Show query performance metrics
  connections Show connection metrics

Examples:
  orochi metrics summary --cluster my-cluster
  orochi metrics cpu --cluster my-cluster --period 24h
  orochi metrics queries --cluster my-cluster --period 1h`,
}

var metricsSummaryCmd = &cobra.Command{
	Use:   "summary",
	Short: "Show metrics summary for a cluster",
	Long: `Show a summary of all key metrics for a cluster.

Examples:
  orochi metrics summary --cluster my-cluster
  orochi metrics summary --cluster my-cluster --output json`,
	RunE: runMetricsSummary,
}

var metricsCPUCmd = &cobra.Command{
	Use:   "cpu",
	Short: "Show CPU metrics",
	Long: `Show CPU utilization metrics for a cluster.

Examples:
  orochi metrics cpu --cluster my-cluster
  orochi metrics cpu --cluster my-cluster --period 24h
  orochi metrics cpu --cluster my-cluster --watch`,
	RunE: runMetricsCPU,
}

var metricsMemoryCmd = &cobra.Command{
	Use:   "memory",
	Short: "Show memory metrics",
	Long: `Show memory utilization metrics for a cluster.

Examples:
  orochi metrics memory --cluster my-cluster
  orochi metrics memory --cluster my-cluster --period 24h`,
	RunE: runMetricsMemory,
}

var metricsStorageCmd = &cobra.Command{
	Use:   "storage",
	Short: "Show storage metrics",
	Long: `Show storage utilization metrics for a cluster.

Examples:
  orochi metrics storage --cluster my-cluster
  orochi metrics storage --cluster my-cluster --period 7d`,
	RunE: runMetricsStorage,
}

var metricsQueriesCmd = &cobra.Command{
	Use:   "queries",
	Short: "Show query performance metrics",
	Long: `Show query performance metrics including slow queries.

Examples:
  orochi metrics queries --cluster my-cluster
  orochi metrics queries --cluster my-cluster --period 1h`,
	RunE: runMetricsQueries,
}

var metricsConnectionsCmd = &cobra.Command{
	Use:   "connections",
	Short: "Show connection metrics",
	Long: `Show connection pool and active connection metrics.

Examples:
  orochi metrics connections --cluster my-cluster
  orochi metrics connections --cluster my-cluster --watch`,
	RunE: runMetricsConnections,
}

func init() {
	// Add subcommands
	metricsCmd.AddCommand(metricsSummaryCmd)
	metricsCmd.AddCommand(metricsCPUCmd)
	metricsCmd.AddCommand(metricsMemoryCmd)
	metricsCmd.AddCommand(metricsStorageCmd)
	metricsCmd.AddCommand(metricsQueriesCmd)
	metricsCmd.AddCommand(metricsConnectionsCmd)

	// Common flags
	addMetricsFlags(metricsSummaryCmd)
	addMetricsFlags(metricsCPUCmd)
	addMetricsFlags(metricsMemoryCmd)
	addMetricsFlags(metricsStorageCmd)
	addMetricsFlags(metricsQueriesCmd)
	addMetricsFlags(metricsConnectionsCmd)

	// Watch mode for specific commands
	metricsCPUCmd.Flags().BoolVarP(&metricsWatchMode, "watch", "w", false, "Watch mode (updates every 5s)")
	metricsConnectionsCmd.Flags().BoolVarP(&metricsWatchMode, "watch", "w", false, "Watch mode (updates every 5s)")
}

func addMetricsFlags(cmd *cobra.Command) {
	cmd.Flags().StringVar(&metricsCluster, "cluster", "", "Cluster name (required)")
	cmd.Flags().StringVar(&metricsPeriod, "period", "1h", "Time period (e.g., 1h, 24h, 7d)")
	cmd.Flags().StringVar(&metricsInterval, "interval", "", "Data point interval (e.g., 1m, 5m, 1h)")
	cmd.MarkFlagRequired("cluster")
}

func runMetricsSummary(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	summary, err := client.GetMetricsSummary(cmd.Context(), metricsCluster, metricsPeriod)
	if err != nil {
		return fmt.Errorf("failed to get metrics summary: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, summary, func() {
		output.PrintMetricsSummary(summary)
	})

	return nil
}

func runMetricsCPU(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	metrics, err := client.GetCPUMetrics(cmd.Context(), metricsCluster, metricsPeriod, metricsInterval)
	if err != nil {
		return fmt.Errorf("failed to get CPU metrics: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, metrics, func() {
		output.PrintCPUMetrics(metrics)
	})

	return nil
}

func runMetricsMemory(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	metrics, err := client.GetMemoryMetrics(cmd.Context(), metricsCluster, metricsPeriod, metricsInterval)
	if err != nil {
		return fmt.Errorf("failed to get memory metrics: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, metrics, func() {
		output.PrintMemoryMetrics(metrics)
	})

	return nil
}

func runMetricsStorage(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	metrics, err := client.GetStorageMetrics(cmd.Context(), metricsCluster, metricsPeriod, metricsInterval)
	if err != nil {
		return fmt.Errorf("failed to get storage metrics: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, metrics, func() {
		output.PrintStorageMetrics(metrics)
	})

	return nil
}

func runMetricsQueries(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	metrics, err := client.GetQueryMetrics(cmd.Context(), metricsCluster, metricsPeriod)
	if err != nil {
		return fmt.Errorf("failed to get query metrics: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, metrics, func() {
		output.PrintQueryMetrics(metrics)
	})

	return nil
}

func runMetricsConnections(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	metrics, err := client.GetConnectionMetrics(cmd.Context(), metricsCluster)
	if err != nil {
		return fmt.Errorf("failed to get connection metrics: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, metrics, func() {
		output.PrintConnectionMetrics(metrics)
	})

	return nil
}
