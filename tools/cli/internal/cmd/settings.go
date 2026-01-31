package cmd

import (
	"fmt"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	settingsCluster string
)

var settingsCmd = &cobra.Command{
	Use:   "settings",
	Short: "Manage cluster settings",
	Long: `View and modify cluster settings.

Commands:
  list      List all settings for a cluster
  get       Get a specific setting value
  set       Set a setting value
  reset     Reset a setting to default value

Examples:
  orochi settings list --cluster my-cluster
  orochi settings get max_connections --cluster my-cluster
  orochi settings set max_connections 200 --cluster my-cluster
  orochi settings reset max_connections --cluster my-cluster`,
}

var settingsListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all settings for a cluster",
	Long: `List all configurable settings for a cluster.

Examples:
  orochi settings list --cluster my-cluster
  orochi settings list --cluster my-cluster --output json`,
	RunE: runSettingsList,
}

var settingsGetCmd = &cobra.Command{
	Use:   "get <setting-name>",
	Short: "Get a specific setting value",
	Long: `Get the current value of a specific setting.

Examples:
  orochi settings get max_connections --cluster my-cluster
  orochi settings get shared_buffers --cluster my-cluster`,
	Args: cobra.ExactArgs(1),
	RunE: runSettingsGet,
}

var settingsSetCmd = &cobra.Command{
	Use:   "set <setting-name> <value>",
	Short: "Set a setting value",
	Long: `Set a configuration setting to a new value.

Some settings require a cluster restart to take effect.

Examples:
  orochi settings set max_connections 200 --cluster my-cluster
  orochi settings set work_mem 256MB --cluster my-cluster
  orochi settings set maintenance_work_mem 1GB --cluster my-cluster`,
	Args: cobra.ExactArgs(2),
	RunE: runSettingsSet,
}

var settingsResetCmd = &cobra.Command{
	Use:   "reset <setting-name>",
	Short: "Reset a setting to default value",
	Long: `Reset a configuration setting to its default value.

Examples:
  orochi settings reset max_connections --cluster my-cluster
  orochi settings reset work_mem --cluster my-cluster`,
	Args: cobra.ExactArgs(1),
	RunE: runSettingsReset,
}

var settingsApplyCmd = &cobra.Command{
	Use:   "apply",
	Short: "Apply pending settings (requires restart)",
	Long: `Apply pending settings that require a cluster restart.

Examples:
  orochi settings apply --cluster my-cluster`,
	RunE: runSettingsApply,
}

func init() {
	// Add subcommands
	settingsCmd.AddCommand(settingsListCmd)
	settingsCmd.AddCommand(settingsGetCmd)
	settingsCmd.AddCommand(settingsSetCmd)
	settingsCmd.AddCommand(settingsResetCmd)
	settingsCmd.AddCommand(settingsApplyCmd)

	// Common cluster flag
	addSettingsClusterFlag(settingsListCmd)
	addSettingsClusterFlag(settingsGetCmd)
	addSettingsClusterFlag(settingsSetCmd)
	addSettingsClusterFlag(settingsResetCmd)
	addSettingsClusterFlag(settingsApplyCmd)
}

func addSettingsClusterFlag(cmd *cobra.Command) {
	cmd.Flags().StringVar(&settingsCluster, "cluster", "", "Cluster name (required)")
	cmd.MarkFlagRequired("cluster")
}

func runSettingsList(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	settings, err := client.ListSettings(cmd.Context(), settingsCluster)
	if err != nil {
		return fmt.Errorf("failed to list settings: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"settings": settings}, func() {
		output.PrintSettingsTable(settings)
	})

	return nil
}

func runSettingsGet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	setting, err := client.GetSetting(cmd.Context(), settingsCluster, name)
	if err != nil {
		return fmt.Errorf("failed to get setting: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, setting, func() {
		output.PrintSettingDetails(setting)
	})

	return nil
}

func runSettingsSet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]
	value := args[1]

	client := api.NewClient()
	setting, err := client.SetSetting(cmd.Context(), settingsCluster, name, value)
	if err != nil {
		return fmt.Errorf("failed to set setting: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(setting)
	} else {
		output.Success("Setting '%s' set to '%s'", name, value)
		if setting.RequiresRestart {
			output.Warning("This setting requires a cluster restart to take effect")
			output.Info("Run 'orochi settings apply --cluster %s' to apply changes", settingsCluster)
		}
	}

	return nil
}

func runSettingsReset(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	setting, err := client.ResetSetting(cmd.Context(), settingsCluster, name)
	if err != nil {
		return fmt.Errorf("failed to reset setting: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(setting)
	} else {
		output.Success("Setting '%s' reset to default value '%s'", name, setting.Value)
		if setting.RequiresRestart {
			output.Warning("This setting requires a cluster restart to take effect")
			output.Info("Run 'orochi settings apply --cluster %s' to apply changes", settingsCluster)
		}
	}

	return nil
}

func runSettingsApply(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	output.Warning("This will restart the cluster to apply pending settings")
	if !output.Confirm("Are you sure you want to continue?") {
		output.Print("Operation cancelled.")
		return nil
	}

	client := api.NewClient()
	if err := client.ApplySettings(cmd.Context(), settingsCluster); err != nil {
		return fmt.Errorf("failed to apply settings: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Settings applied. Cluster '%s' is restarting.", settingsCluster))

	return nil
}

// Ensure strings package is used
var _ = strings.TrimSpace
