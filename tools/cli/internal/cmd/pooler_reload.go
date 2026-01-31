package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var poolerReloadCmd = &cobra.Command{
	Use:   "reload <cluster-id>",
	Short: "Reload pooler configuration",
	Long: `Reload the PgDog connection pooler configuration without restart.

This command triggers a configuration reload on the pooler, applying any
configuration changes that were made via 'orochi cluster pooler config set'.

The reload is performed gracefully:
- Active connections are not interrupted
- New connections use the updated configuration
- Configuration changes take effect immediately

Note: Some configuration changes may require a full restart rather than
a reload. In such cases, the command will indicate this.

Examples:
  # Reload pooler configuration
  orochi cluster pooler reload my-cluster

  # Reload after making config changes
  orochi cluster pooler config set my-cluster --mode transaction
  orochi cluster pooler reload my-cluster`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerReload,
}

func runPoolerReload(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]

	// Show spinner for long operation
	spinner := output.NewSpinner(fmt.Sprintf("Reloading pooler configuration for '%s'...", clusterID))
	spinner.Start()

	client := api.NewClient()
	err := client.ReloadPoolerConfig(cmd.Context(), clusterID)

	spinner.Stop()

	if err != nil {
		return fmt.Errorf("failed to reload pooler config: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]interface{}{
			"status":     "success",
			"cluster_id": clusterID,
			"message":    "Pooler configuration reloaded successfully",
		})
	} else {
		output.Success("Pooler configuration reloaded successfully for cluster '%s'", clusterID)
		output.Info("Run 'orochi cluster pooler status %s' to verify", clusterID)
	}

	return nil
}
