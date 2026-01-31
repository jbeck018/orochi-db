package cmd

import (
	"fmt"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

// Config set flags
var (
	configMode            string
	configMaxPoolSize     int
	configMinPoolSize     int
	configIdleTimeout     int
	configConnectionLimit int
	configReadWriteSplit  string // "true", "false", or ""
	configShardingEnabled string // "true", "false", or ""
	configShardCount      int
	configLoadBalancing   string
	configQueryTimeout    int
	configBanTime         int
)

var poolerConfigCmd = &cobra.Command{
	Use:   "config",
	Short: "View and update pooler configuration",
	Long: `View and update the PgDog connection pooler configuration.

Subcommands:
  get    Get current pooler configuration
  set    Update pooler configuration

Configuration Options:
  --mode              Pooling mode: session, transaction, or statement
  --max-pool-size     Maximum connections in the pool
  --min-pool-size     Minimum connections to maintain
  --idle-timeout      Seconds before closing idle connections
  --read-write-split  Enable read/write splitting (true/false)
  --sharding-enabled  Enable query sharding (true/false)
  --shard-count       Number of shards (requires sharding enabled)
  --load-balancing    Load balancing strategy
  --query-timeout     Query timeout in seconds
  --ban-time          Connection ban time in seconds

Examples:
  # View current configuration
  orochi cluster pooler config get my-cluster

  # Change pooling mode to transaction
  orochi cluster pooler config set my-cluster --mode transaction

  # Update pool size limits
  orochi cluster pooler config set my-cluster --max-pool-size 100 --min-pool-size 10

  # Enable read/write splitting
  orochi cluster pooler config set my-cluster --read-write-split true

  # Configure sharding
  orochi cluster pooler config set my-cluster --sharding-enabled true --shard-count 16`,
}

var poolerConfigGetCmd = &cobra.Command{
	Use:   "get <cluster-id>",
	Short: "Get current pooler configuration",
	Long: `Get the current configuration of the PgDog connection pooler.

Displays all configuration options including:
- Pooling mode (session, transaction, statement)
- Pool size limits (min, max)
- Timeout settings
- Read/write splitting configuration
- Sharding configuration
- Load balancing strategy

Examples:
  # View configuration
  orochi cluster pooler config get my-cluster

  # Output as JSON
  orochi cluster pooler config get my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerConfigGet,
}

var poolerConfigSetCmd = &cobra.Command{
	Use:   "set <cluster-id> [flags]",
	Short: "Update pooler configuration",
	Long: `Update the PgDog connection pooler configuration.

Changes are applied after running 'orochi cluster pooler reload'.

Pooling Modes:
  session     - Client keeps same backend connection (default)
  transaction - Backend released after transaction commit
  statement   - Backend released after each statement

Load Balancing Strategies:
  round_robin           - Distribute evenly across backends
  random                - Random backend selection
  least_connections     - Backend with fewest connections
  least_outstanding     - Backend with fewest active queries
  query_parser_weighted - Route based on query analysis

Examples:
  # Change pooling mode
  orochi cluster pooler config set my-cluster --mode transaction

  # Update pool size
  orochi cluster pooler config set my-cluster --max-pool-size 200 --min-pool-size 20

  # Enable read/write splitting with custom load balancing
  orochi cluster pooler config set my-cluster --read-write-split true --load-balancing least_connections

  # Configure sharding
  orochi cluster pooler config set my-cluster --sharding-enabled true --shard-count 8

  # Set timeouts
  orochi cluster pooler config set my-cluster --idle-timeout 300 --query-timeout 60`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerConfigSet,
}

func init() {
	poolerConfigCmd.AddCommand(poolerConfigGetCmd)
	poolerConfigCmd.AddCommand(poolerConfigSetCmd)

	// Config set flags
	poolerConfigSetCmd.Flags().StringVar(&configMode, "mode", "", "Pooling mode (session, transaction, statement)")
	poolerConfigSetCmd.Flags().IntVar(&configMaxPoolSize, "max-pool-size", 0, "Maximum connections in the pool")
	poolerConfigSetCmd.Flags().IntVar(&configMinPoolSize, "min-pool-size", 0, "Minimum connections to maintain")
	poolerConfigSetCmd.Flags().IntVar(&configIdleTimeout, "idle-timeout", 0, "Idle timeout in seconds")
	poolerConfigSetCmd.Flags().IntVar(&configConnectionLimit, "connection-limit", 0, "Maximum client connections")
	poolerConfigSetCmd.Flags().StringVar(&configReadWriteSplit, "read-write-split", "", "Enable read/write splitting (true/false)")
	poolerConfigSetCmd.Flags().StringVar(&configShardingEnabled, "sharding-enabled", "", "Enable query sharding (true/false)")
	poolerConfigSetCmd.Flags().IntVar(&configShardCount, "shard-count", 0, "Number of shards")
	poolerConfigSetCmd.Flags().StringVar(&configLoadBalancing, "load-balancing", "", "Load balancing strategy")
	poolerConfigSetCmd.Flags().IntVar(&configQueryTimeout, "query-timeout", 0, "Query timeout in seconds")
	poolerConfigSetCmd.Flags().IntVar(&configBanTime, "ban-time", 0, "Connection ban time in seconds")
}

func runPoolerConfigGet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]

	client := api.NewClient()
	config, err := client.GetPoolerConfig(cmd.Context(), clusterID)
	if err != nil {
		return fmt.Errorf("failed to get pooler config: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, config, func() {
		printPoolerConfig(clusterID, config)
	})

	return nil
}

func runPoolerConfigSet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]

	// Build update request from flags
	update := &api.PoolerConfigUpdate{}
	hasChanges := false

	if configMode != "" {
		mode, err := api.ValidatePoolerMode(configMode)
		if err != nil {
			return err
		}
		update.Mode = &mode
		hasChanges = true
	}

	if configMaxPoolSize > 0 {
		update.MaxPoolSize = &configMaxPoolSize
		hasChanges = true
	}

	if configMinPoolSize > 0 {
		update.MinPoolSize = &configMinPoolSize
		hasChanges = true
	}

	if configIdleTimeout > 0 {
		update.IdleTimeout = &configIdleTimeout
		hasChanges = true
	}

	if configConnectionLimit > 0 {
		update.ConnectionLimit = &configConnectionLimit
		hasChanges = true
	}

	if configReadWriteSplit != "" {
		val, err := parseBoolFlag(configReadWriteSplit)
		if err != nil {
			return fmt.Errorf("invalid value for --read-write-split: %w", err)
		}
		update.ReadWriteSplit = &val
		hasChanges = true
	}

	if configShardingEnabled != "" {
		val, err := parseBoolFlag(configShardingEnabled)
		if err != nil {
			return fmt.Errorf("invalid value for --sharding-enabled: %w", err)
		}
		update.ShardingEnabled = &val
		hasChanges = true
	}

	if configShardCount > 0 {
		update.ShardCount = &configShardCount
		hasChanges = true
	}

	if configLoadBalancing != "" {
		if err := api.ValidateLoadBalancing(configLoadBalancing); err != nil {
			return err
		}
		update.LoadBalancing = &configLoadBalancing
		hasChanges = true
	}

	if configQueryTimeout > 0 {
		update.QueryTimeout = &configQueryTimeout
		hasChanges = true
	}

	if configBanTime > 0 {
		update.BanTime = &configBanTime
		hasChanges = true
	}

	if !hasChanges {
		return fmt.Errorf("no configuration changes specified. Use flags like --mode, --max-pool-size, etc.")
	}

	// Validate pool size relationship
	if update.MinPoolSize != nil && update.MaxPoolSize != nil {
		if *update.MinPoolSize > *update.MaxPoolSize {
			return fmt.Errorf("min-pool-size (%d) cannot be greater than max-pool-size (%d)",
				*update.MinPoolSize, *update.MaxPoolSize)
		}
	}

	client := api.NewClient()
	config, err := client.UpdatePoolerConfig(cmd.Context(), clusterID, update)
	if err != nil {
		return fmt.Errorf("failed to update pooler config: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(config)
	} else {
		output.Success("Pooler configuration updated for cluster '%s'", clusterID)
		fmt.Println()
		printPoolerConfig(clusterID, config)
		fmt.Println()
		output.Info("Run 'orochi cluster pooler reload %s' to apply changes", clusterID)
	}

	return nil
}

func printPoolerConfig(clusterID string, config *api.PoolerConfig) {
	output.BoldColor.Printf("Pooler Configuration: %s\n", clusterID)
	fmt.Println(strings.Repeat("=", 50))
	fmt.Println()

	// Connection pooling section
	output.BoldColor.Println("Connection Pooling")
	fmt.Println(strings.Repeat("-", 50))

	printConfigField("Mode", string(config.Mode))
	printConfigField("Max Pool Size", fmt.Sprintf("%d", config.MaxPoolSize))
	printConfigField("Min Pool Size", fmt.Sprintf("%d", config.MinPoolSize))
	printConfigField("Connection Limit", fmt.Sprintf("%d", config.ConnectionLimit))

	fmt.Println()

	// Timeout section
	output.BoldColor.Println("Timeouts")
	fmt.Println(strings.Repeat("-", 50))

	printConfigField("Idle Timeout", fmt.Sprintf("%d seconds", config.IdleTimeout))
	printConfigField("Query Timeout", fmt.Sprintf("%d seconds", config.QueryTimeout))
	printConfigField("Ban Time", fmt.Sprintf("%d seconds", config.BanTime))

	fmt.Println()

	// Routing section
	output.BoldColor.Println("Routing & Sharding")
	fmt.Println(strings.Repeat("-", 50))

	printConfigField("Read/Write Split", formatBool(config.ReadWriteSplit))
	printConfigField("Load Balancing", config.LoadBalancing)

	if config.ShardingEnabled {
		printConfigField("Sharding", fmt.Sprintf("Enabled (%d shards)", config.ShardCount))
	} else {
		printConfigField("Sharding", "Disabled")
	}
}

func printConfigField(label, value string) {
	output.DimColor.Printf("%-20s", label+":")
	fmt.Printf(" %s\n", value)
}

func parseBoolFlag(value string) (bool, error) {
	switch strings.ToLower(value) {
	case "true", "yes", "1", "on", "enabled":
		return true, nil
	case "false", "no", "0", "off", "disabled":
		return false, nil
	default:
		return false, fmt.Errorf("must be 'true' or 'false', got '%s'", value)
	}
}
