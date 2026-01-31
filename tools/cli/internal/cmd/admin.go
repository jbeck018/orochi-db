package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	adminCluster string
)

var adminCmd = &cobra.Command{
	Use:   "admin",
	Short: "Administrative operations",
	Long: `Perform administrative operations on clusters.

Commands:
  restart       Restart a cluster
  maintenance   Enable/disable maintenance mode
  upgrade       Upgrade cluster to a new version
  logs          View cluster logs
  events        View cluster events
  audit         View audit logs

Examples:
  orochi admin restart --cluster my-cluster
  orochi admin maintenance enable --cluster my-cluster
  orochi admin upgrade --cluster my-cluster --version 17.2
  orochi admin logs --cluster my-cluster --tail 100
  orochi admin events --cluster my-cluster`,
}

var adminRestartCmd = &cobra.Command{
	Use:   "restart",
	Short: "Restart a cluster",
	Long: `Restart a cluster. This will cause a brief downtime.

Examples:
  orochi admin restart --cluster my-cluster
  orochi admin restart --cluster my-cluster --force`,
	RunE: runAdminRestart,
}

var adminMaintenanceCmd = &cobra.Command{
	Use:   "maintenance <enable|disable>",
	Short: "Enable or disable maintenance mode",
	Long: `Enable or disable maintenance mode on a cluster.

When maintenance mode is enabled:
- New connections are rejected
- Existing connections remain active
- Background jobs continue running

Examples:
  orochi admin maintenance enable --cluster my-cluster
  orochi admin maintenance disable --cluster my-cluster`,
	Args: cobra.ExactArgs(1),
	RunE: runAdminMaintenance,
}

var adminUpgradeCmd = &cobra.Command{
	Use:   "upgrade",
	Short: "Upgrade cluster to a new version",
	Long: `Upgrade a cluster to a new PostgreSQL version.

Available versions can be checked with 'orochi admin versions'.

Examples:
  orochi admin upgrade --cluster my-cluster --version 17.2
  orochi admin upgrade --cluster my-cluster --version 18.0 --schedule "2024-01-15 02:00"`,
	RunE: runAdminUpgrade,
}

var adminLogsCmd = &cobra.Command{
	Use:   "logs",
	Short: "View cluster logs",
	Long: `View PostgreSQL and system logs for a cluster.

Examples:
  orochi admin logs --cluster my-cluster
  orochi admin logs --cluster my-cluster --tail 100
  orochi admin logs --cluster my-cluster --since 1h
  orochi admin logs --cluster my-cluster --level error`,
	RunE: runAdminLogs,
}

var adminEventsCmd = &cobra.Command{
	Use:   "events",
	Short: "View cluster events",
	Long: `View events (scaling, failovers, maintenance) for a cluster.

Examples:
  orochi admin events --cluster my-cluster
  orochi admin events --cluster my-cluster --since 24h`,
	RunE: runAdminEvents,
}

var adminAuditCmd = &cobra.Command{
	Use:   "audit",
	Short: "View audit logs",
	Long: `View audit logs for a cluster (queries, connections, DDL).

Examples:
  orochi admin audit --cluster my-cluster
  orochi admin audit --cluster my-cluster --since 1h
  orochi admin audit --cluster my-cluster --type query`,
	RunE: runAdminAudit,
}

var adminVersionsCmd = &cobra.Command{
	Use:   "versions",
	Short: "List available PostgreSQL versions",
	Long: `List available PostgreSQL versions for upgrades.

Examples:
  orochi admin versions
  orochi admin versions --output json`,
	RunE: runAdminVersions,
}

var (
	adminVersion   string
	adminSchedule  string
	adminTail      int
	adminSince     string
	adminLevel     string
	adminAuditType string
)

func init() {
	// Add subcommands
	adminCmd.AddCommand(adminRestartCmd)
	adminCmd.AddCommand(adminMaintenanceCmd)
	adminCmd.AddCommand(adminUpgradeCmd)
	adminCmd.AddCommand(adminLogsCmd)
	adminCmd.AddCommand(adminEventsCmd)
	adminCmd.AddCommand(adminAuditCmd)
	adminCmd.AddCommand(adminVersionsCmd)

	// Restart flags
	adminRestartCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminRestartCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force restart without confirmation")
	adminRestartCmd.MarkFlagRequired("cluster")

	// Maintenance flags
	adminMaintenanceCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminMaintenanceCmd.MarkFlagRequired("cluster")

	// Upgrade flags
	adminUpgradeCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminUpgradeCmd.Flags().StringVar(&adminVersion, "version", "", "Target PostgreSQL version (required)")
	adminUpgradeCmd.Flags().StringVar(&adminSchedule, "schedule", "", "Schedule upgrade for later (format: YYYY-MM-DD HH:MM)")
	adminUpgradeCmd.MarkFlagRequired("cluster")
	adminUpgradeCmd.MarkFlagRequired("version")

	// Logs flags
	adminLogsCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminLogsCmd.Flags().IntVar(&adminTail, "tail", 50, "Number of lines to show")
	adminLogsCmd.Flags().StringVar(&adminSince, "since", "", "Show logs since duration (e.g., 1h, 24h)")
	adminLogsCmd.Flags().StringVar(&adminLevel, "level", "", "Filter by log level (debug, info, warning, error)")
	adminLogsCmd.MarkFlagRequired("cluster")

	// Events flags
	adminEventsCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminEventsCmd.Flags().StringVar(&adminSince, "since", "24h", "Show events since duration")
	adminEventsCmd.MarkFlagRequired("cluster")

	// Audit flags
	adminAuditCmd.Flags().StringVar(&adminCluster, "cluster", "", "Cluster name (required)")
	adminAuditCmd.Flags().StringVar(&adminSince, "since", "1h", "Show audit logs since duration")
	adminAuditCmd.Flags().StringVar(&adminAuditType, "type", "", "Filter by type (query, connection, ddl)")
	adminAuditCmd.MarkFlagRequired("cluster")
}

func runAdminRestart(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	if !forceDelete {
		output.Warning("You are about to restart cluster '%s'", adminCluster)
		output.Print("This will cause a brief downtime (typically 30-60 seconds).")
		if !output.Confirm("Are you sure?") {
			output.Print("Restart cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.RestartCluster(cmd.Context(), adminCluster); err != nil {
		return fmt.Errorf("failed to restart cluster: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Cluster '%s' is restarting", adminCluster))

	return nil
}

func runAdminMaintenance(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	action := args[0]
	if action != "enable" && action != "disable" {
		return fmt.Errorf("invalid action: %s (must be 'enable' or 'disable')", action)
	}

	enable := action == "enable"

	client := api.NewClient()
	if err := client.SetMaintenanceMode(cmd.Context(), adminCluster, enable); err != nil {
		return fmt.Errorf("failed to %s maintenance mode: %w", action, err)
	}

	format := getOutputFormat()
	if enable {
		output.PrintSuccess(format, fmt.Sprintf("Maintenance mode enabled for '%s'", adminCluster))
	} else {
		output.PrintSuccess(format, fmt.Sprintf("Maintenance mode disabled for '%s'", adminCluster))
	}

	return nil
}

func runAdminUpgrade(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	output.Warning("You are about to upgrade cluster '%s' to PostgreSQL %s", adminCluster, adminVersion)
	if adminSchedule != "" {
		output.Print("Upgrade will be scheduled for: %s", adminSchedule)
	} else {
		output.Print("Upgrade will start immediately and may cause downtime.")
	}
	if !output.Confirm("Are you sure?") {
		output.Print("Upgrade cancelled.")
		return nil
	}

	client := api.NewClient()
	upgrade, err := client.UpgradeCluster(cmd.Context(), adminCluster, adminVersion, adminSchedule)
	if err != nil {
		return fmt.Errorf("failed to schedule upgrade: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(upgrade)
	} else {
		if adminSchedule != "" {
			output.Success("Upgrade scheduled for '%s' at %s", adminCluster, adminSchedule)
		} else {
			output.Success("Upgrade started for '%s'", adminCluster)
			output.Info("Run 'orochi cluster get %s' to check progress", adminCluster)
		}
	}

	return nil
}

func runAdminLogs(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	logs, err := client.GetClusterLogs(cmd.Context(), adminCluster, &api.LogsRequest{
		Tail:  adminTail,
		Since: adminSince,
		Level: adminLevel,
	})
	if err != nil {
		return fmt.Errorf("failed to get logs: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"logs": logs}, func() {
		output.PrintLogs(logs)
	})

	return nil
}

func runAdminEvents(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	events, err := client.GetClusterEvents(cmd.Context(), adminCluster, adminSince)
	if err != nil {
		return fmt.Errorf("failed to get events: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"events": events}, func() {
		output.PrintEvents(events)
	})

	return nil
}

func runAdminAudit(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	auditLogs, err := client.GetAuditLogs(cmd.Context(), adminCluster, &api.AuditRequest{
		Since: adminSince,
		Type:  adminAuditType,
	})
	if err != nil {
		return fmt.Errorf("failed to get audit logs: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"audit_logs": auditLogs}, func() {
		output.PrintAuditLogs(auditLogs)
	})

	return nil
}

func runAdminVersions(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	versions, err := client.GetAvailableVersions(cmd.Context())
	if err != nil {
		return fmt.Errorf("failed to get versions: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"versions": versions}, func() {
		output.PrintVersionsTable(versions)
	})

	return nil
}
