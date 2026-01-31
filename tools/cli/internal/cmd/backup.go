package cmd

import (
	"fmt"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	backupCluster string
	backupRetain  int
)

var backupCmd = &cobra.Command{
	Use:   "backup",
	Short: "Manage cluster backups",
	Long: `Manage backups for your Orochi Cloud clusters.

Commands:
  list      List all backups for a cluster
  create    Create a new backup
  restore   Restore from a backup
  delete    Delete a backup

Examples:
  orochi backup list --cluster my-cluster
  orochi backup create --cluster my-cluster
  orochi backup restore <backup-id> --cluster my-cluster
  orochi backup delete <backup-id>`,
}

var backupListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all backups for a cluster",
	Long: `List all backups for a specific Orochi Cloud cluster.

Examples:
  orochi backup list --cluster my-cluster
  orochi backup list --cluster my-cluster --output json`,
	RunE: runBackupList,
}

var backupCreateCmd = &cobra.Command{
	Use:   "create",
	Short: "Create a new backup",
	Long: `Create a new backup for an Orochi Cloud cluster.

Examples:
  orochi backup create --cluster my-cluster
  orochi backup create --cluster my-cluster --retain 30`,
	RunE: runBackupCreate,
}

var backupRestoreCmd = &cobra.Command{
	Use:   "restore <backup-id>",
	Short: "Restore from a backup",
	Long: `Restore a cluster from a specific backup.

Examples:
  orochi backup restore bkp-abc123 --cluster my-cluster
  orochi backup restore bkp-abc123 --cluster new-cluster`,
	Args: cobra.ExactArgs(1),
	RunE: runBackupRestore,
}

var backupDeleteCmd = &cobra.Command{
	Use:   "delete <backup-id>",
	Short: "Delete a backup",
	Long: `Delete a specific backup.

Examples:
  orochi backup delete bkp-abc123
  orochi backup delete bkp-abc123 --force`,
	Args: cobra.ExactArgs(1),
	RunE: runBackupDelete,
}

func init() {
	// Add subcommands
	backupCmd.AddCommand(backupListCmd)
	backupCmd.AddCommand(backupCreateCmd)
	backupCmd.AddCommand(backupRestoreCmd)
	backupCmd.AddCommand(backupDeleteCmd)

	// Common flags
	backupListCmd.Flags().StringVar(&backupCluster, "cluster", "", "Cluster name (required)")
	backupListCmd.MarkFlagRequired("cluster")

	backupCreateCmd.Flags().StringVar(&backupCluster, "cluster", "", "Cluster name (required)")
	backupCreateCmd.Flags().IntVar(&backupRetain, "retain", 7, "Days to retain backup")
	backupCreateCmd.MarkFlagRequired("cluster")

	backupRestoreCmd.Flags().StringVar(&backupCluster, "cluster", "", "Target cluster name (required)")
	backupRestoreCmd.MarkFlagRequired("cluster")

	backupDeleteCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force delete without confirmation")
}

func runBackupList(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	backups, err := client.ListBackups(cmd.Context(), backupCluster)
	if err != nil {
		return fmt.Errorf("failed to list backups: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"backups": backups}, func() {
		output.PrintBackupTable(backups)
	})

	return nil
}

func runBackupCreate(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	backup, err := client.CreateBackup(cmd.Context(), backupCluster, backupRetain)
	if err != nil {
		return fmt.Errorf("failed to create backup: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(backup)
	} else {
		output.Success("Backup '%s' is being created for cluster '%s'", backup.ID, backupCluster)
		output.Info("Run 'orochi backup list --cluster %s' to check status", backupCluster)
	}

	return nil
}

func runBackupRestore(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	backupID := args[0]

	client := api.NewClient()
	cluster, err := client.RestoreBackup(cmd.Context(), backupID, backupCluster)
	if err != nil {
		return fmt.Errorf("failed to restore backup: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(cluster)
	} else {
		output.Success("Restoring backup '%s' to cluster '%s'", backupID, backupCluster)
		output.Info("Run 'orochi cluster get %s' to check status", backupCluster)
	}

	return nil
}

func runBackupDelete(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	backupID := args[0]

	if !forceDelete {
		output.Warning("You are about to delete backup '%s'", backupID)
		if !output.Confirm("Are you sure?") {
			output.Print("Deletion cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.DeleteBackup(cmd.Context(), backupID); err != nil {
		return fmt.Errorf("failed to delete backup: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Backup '%s' deleted", backupID))

	return nil
}

// Ensure strings package is used
var _ = strings.TrimSpace
