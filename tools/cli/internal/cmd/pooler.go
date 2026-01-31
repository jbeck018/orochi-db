package cmd

import (
	"github.com/spf13/cobra"
)

// poolerCmd represents the pooler command group.
var poolerCmd = &cobra.Command{
	Use:   "pooler",
	Short: "Manage PgDog connection pooler",
	Long: `Manage the PgDog connection pooler for your Orochi Cloud cluster.

PgDog is a high-performance PostgreSQL connection pooler that provides:
- Connection pooling with session, transaction, and statement modes
- Read/write splitting for replica routing
- Sharding support for distributed queries
- Real-time monitoring and statistics

Commands:
  status    Show pooler status and health
  config    View and update pooler configuration
  stats     View detailed pooler statistics
  reload    Reload pooler configuration
  clients   List connected clients

Examples:
  # Check pooler status
  orochi cluster pooler status my-cluster

  # View pooler configuration
  orochi cluster pooler config get my-cluster

  # Update pooler mode to transaction
  orochi cluster pooler config set my-cluster --mode transaction

  # View real-time statistics
  orochi cluster pooler stats my-cluster --interval 5s

  # List all connected clients
  orochi cluster pooler clients my-cluster

  # Reload configuration without restart
  orochi cluster pooler reload my-cluster`,
}

func init() {
	// Add subcommands to pooler
	poolerCmd.AddCommand(poolerStatusCmd)
	poolerCmd.AddCommand(poolerConfigCmd)
	poolerCmd.AddCommand(poolerStatsCmd)
	poolerCmd.AddCommand(poolerReloadCmd)
	poolerCmd.AddCommand(poolerClientsCmd)
}
