package cmd

import (
	"bufio"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

// Cluster command flags
var (
	clusterSize     string
	clusterRegion   string
	clusterReplicas int
	forceDelete     bool
)

var clusterCmd = &cobra.Command{
	Use:   "cluster",
	Short: "Manage Orochi Cloud clusters",
	Long: `Manage your Orochi Cloud database clusters.

Commands:
  list      List all clusters
  create    Create a new cluster
  get       Get cluster details
  delete    Delete a cluster
  scale     Scale a cluster
  connect   Get connection string

Examples:
  orochi cluster list
  orochi cluster create my-cluster --size small --region us-east-1
  orochi cluster get my-cluster
  orochi cluster scale my-cluster --replicas 3
  orochi cluster connect my-cluster
  orochi cluster delete my-cluster`,
}

var clusterListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all clusters",
	Long: `List all Orochi Cloud clusters in your account.

Examples:
  orochi cluster list
  orochi cluster list --output json`,
	RunE: runClusterList,
}

var clusterCreateCmd = &cobra.Command{
	Use:   "create <name>",
	Short: "Create a new cluster",
	Long: `Create a new Orochi Cloud cluster.

Cluster sizes:
  small   - 2 vCPU, 4 GB RAM, 50 GB storage
  medium  - 4 vCPU, 8 GB RAM, 100 GB storage
  large   - 8 vCPU, 16 GB RAM, 250 GB storage
  xlarge  - 16 vCPU, 32 GB RAM, 500 GB storage

Available regions:
  us-east-1, us-west-2, eu-west-1, eu-central-1, ap-southeast-1

Examples:
  orochi cluster create my-cluster --size small --region us-east-1
  orochi cluster create prod-db --size large --region eu-west-1 --replicas 3`,
	Args: cobra.ExactArgs(1),
	RunE: runClusterCreate,
}

var clusterGetCmd = &cobra.Command{
	Use:   "get <name>",
	Short: "Get cluster details",
	Long: `Get detailed information about a specific cluster.

Examples:
  orochi cluster get my-cluster
  orochi cluster get my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runClusterGet,
}

var clusterDeleteCmd = &cobra.Command{
	Use:   "delete <name>",
	Short: "Delete a cluster",
	Long: `Delete an Orochi Cloud cluster.

WARNING: This action is irreversible. All data will be permanently deleted.

Examples:
  orochi cluster delete my-cluster
  orochi cluster delete my-cluster --force`,
	Args: cobra.ExactArgs(1),
	RunE: runClusterDelete,
}

var clusterScaleCmd = &cobra.Command{
	Use:   "scale <name>",
	Short: "Scale a cluster",
	Long: `Scale an Orochi Cloud cluster by changing the number of replicas.

Examples:
  orochi cluster scale my-cluster --replicas 3
  orochi cluster scale my-cluster --replicas 5`,
	Args: cobra.ExactArgs(1),
	RunE: runClusterScale,
}

var clusterConnectCmd = &cobra.Command{
	Use:   "connect <name>",
	Short: "Get connection string",
	Long: `Get the connection string for an Orochi Cloud cluster.

The connection string can be used with psql, your application, or any
PostgreSQL-compatible client.

Examples:
  orochi cluster connect my-cluster
  orochi cluster connect my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runClusterConnect,
}

func init() {
	// Add subcommands to cluster
	clusterCmd.AddCommand(clusterListCmd)
	clusterCmd.AddCommand(clusterCreateCmd)
	clusterCmd.AddCommand(clusterGetCmd)
	clusterCmd.AddCommand(clusterDeleteCmd)
	clusterCmd.AddCommand(clusterScaleCmd)
	clusterCmd.AddCommand(clusterConnectCmd)

	// Create command flags
	clusterCreateCmd.Flags().StringVar(&clusterSize, "size", "small", "Cluster size (small, medium, large, xlarge)")
	clusterCreateCmd.Flags().StringVar(&clusterRegion, "region", "us-east-1", "Cluster region")
	clusterCreateCmd.Flags().IntVar(&clusterReplicas, "replicas", 1, "Number of replicas")

	// Scale command flags
	clusterScaleCmd.Flags().IntVar(&clusterReplicas, "replicas", 0, "Number of replicas")
	clusterScaleCmd.MarkFlagRequired("replicas")

	// Delete command flags
	clusterDeleteCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force delete without confirmation")
}

func runClusterList(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	clusters, err := client.ListClusters(cmd.Context())
	if err != nil {
		return fmt.Errorf("failed to list clusters: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"clusters": clusters}, func() {
		output.PrintClusterTable(clusters)
	})

	return nil
}

func runClusterCreate(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	// Validate cluster size
	size, err := api.ValidateClusterSize(clusterSize)
	if err != nil {
		return err
	}

	// Validate replicas
	if clusterReplicas < 1 || clusterReplicas > 10 {
		return fmt.Errorf("replicas must be between 1 and 10")
	}

	// Show what will be created
	vcpu, memGB, storageGB := api.GetClusterSpecs(size)
	output.Info("Creating cluster '%s':", name)
	output.Print("  Size:     %s (%d vCPU, %d GB RAM)", size, vcpu, memGB)
	output.Print("  Region:   %s", clusterRegion)
	output.Print("  Replicas: %d", clusterReplicas)
	output.Print("  Storage:  %d GB", storageGB)
	output.Print("")

	client := api.NewClient()
	cluster, err := client.CreateCluster(cmd.Context(), &api.CreateClusterRequest{
		Name:     name,
		Size:     size,
		Region:   clusterRegion,
		Replicas: clusterReplicas,
	})
	if err != nil {
		return fmt.Errorf("failed to create cluster: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(cluster)
	} else {
		output.Success("Cluster '%s' is being created", cluster.Name)
		output.Info("Run 'orochi cluster get %s' to check status", cluster.Name)
	}

	return nil
}

func runClusterGet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	cluster, err := client.GetCluster(cmd.Context(), name)
	if err != nil {
		return fmt.Errorf("failed to get cluster: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, cluster, func() {
		output.PrintClusterDetails(cluster)
	})

	return nil
}

func runClusterDelete(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	// Confirm deletion unless --force is used
	if !forceDelete {
		output.Warning("You are about to delete cluster '%s'", name)
		output.Print("This action is irreversible. All data will be permanently deleted.")
		output.Print("")

		reader := bufio.NewReader(os.Stdin)
		fmt.Printf("Type the cluster name to confirm deletion: ")
		confirmation, err := reader.ReadString('\n')
		if err != nil {
			return fmt.Errorf("failed to read confirmation: %w", err)
		}

		confirmation = strings.TrimSpace(confirmation)
		if confirmation != name {
			output.Error("Cluster name does not match. Deletion cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.DeleteCluster(cmd.Context(), name); err != nil {
		return fmt.Errorf("failed to delete cluster: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Cluster '%s' is being deleted", name))

	return nil
}

func runClusterScale(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	// Validate replicas
	if clusterReplicas < 1 || clusterReplicas > 10 {
		return fmt.Errorf("replicas must be between 1 and 10")
	}

	client := api.NewClient()
	cluster, err := client.ScaleCluster(cmd.Context(), name, clusterReplicas)
	if err != nil {
		return fmt.Errorf("failed to scale cluster: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(cluster)
	} else {
		output.Success("Cluster '%s' is scaling to %d replicas", name, clusterReplicas)
		output.Info("Run 'orochi cluster get %s' to check status", name)
	}

	return nil
}

func runClusterConnect(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	connStr, err := client.GetConnectionString(cmd.Context(), name)
	if err != nil {
		return fmt.Errorf("failed to get connection string: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]string{
			"cluster":           name,
			"connection_string": connStr,
		})
	} else {
		output.PrintConnectionString(connStr)
	}

	return nil
}
