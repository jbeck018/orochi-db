package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	branchCluster  string
	branchParent   string
	branchEndpoint bool
)

var branchCmd = &cobra.Command{
	Use:   "branch",
	Short: "Manage database branches",
	Long: `Manage database branches for development and testing.

Branches are copy-on-write snapshots of your database that allow you to
create isolated development environments without duplicating data.

Commands:
  list      List all branches for a cluster
  create    Create a new branch
  get       Get branch details
  delete    Delete a branch
  connect   Get connection string for a branch
  reset     Reset a branch to parent state

Examples:
  orochi branch list --cluster my-cluster
  orochi branch create dev --cluster my-cluster
  orochi branch create feature-x --cluster my-cluster --parent dev
  orochi branch connect dev --cluster my-cluster
  orochi branch delete dev --cluster my-cluster`,
}

var branchListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all branches for a cluster",
	Long: `List all branches for a specific Orochi Cloud cluster.

Examples:
  orochi branch list --cluster my-cluster
  orochi branch list --cluster my-cluster --output json`,
	RunE: runBranchList,
}

var branchCreateCmd = &cobra.Command{
	Use:   "create <name>",
	Short: "Create a new branch",
	Long: `Create a new branch from the main database or another branch.

Branches use copy-on-write storage, so they're instant and space-efficient.

Examples:
  orochi branch create dev --cluster my-cluster
  orochi branch create feature-x --cluster my-cluster --parent dev
  orochi branch create staging --cluster my-cluster --endpoint`,
	Args: cobra.ExactArgs(1),
	RunE: runBranchCreate,
}

var branchGetCmd = &cobra.Command{
	Use:   "get <name>",
	Short: "Get branch details",
	Long: `Get detailed information about a specific branch.

Examples:
  orochi branch get dev --cluster my-cluster
  orochi branch get dev --cluster my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runBranchGet,
}

var branchDeleteCmd = &cobra.Command{
	Use:   "delete <name>",
	Short: "Delete a branch",
	Long: `Delete a branch. This cannot be undone.

Examples:
  orochi branch delete dev --cluster my-cluster
  orochi branch delete dev --cluster my-cluster --force`,
	Args: cobra.ExactArgs(1),
	RunE: runBranchDelete,
}

var branchConnectCmd = &cobra.Command{
	Use:   "connect <name>",
	Short: "Get connection string for a branch",
	Long: `Get the connection string for connecting to a branch.

Examples:
  orochi branch connect dev --cluster my-cluster
  orochi branch connect dev --cluster my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runBranchConnect,
}

var branchResetCmd = &cobra.Command{
	Use:   "reset <name>",
	Short: "Reset a branch to parent state",
	Long: `Reset a branch to match its parent's current state.

WARNING: This will discard all changes made to the branch.

Examples:
  orochi branch reset dev --cluster my-cluster
  orochi branch reset dev --cluster my-cluster --force`,
	Args: cobra.ExactArgs(1),
	RunE: runBranchReset,
}

func init() {
	// Add subcommands
	branchCmd.AddCommand(branchListCmd)
	branchCmd.AddCommand(branchCreateCmd)
	branchCmd.AddCommand(branchGetCmd)
	branchCmd.AddCommand(branchDeleteCmd)
	branchCmd.AddCommand(branchConnectCmd)
	branchCmd.AddCommand(branchResetCmd)

	// Common cluster flag
	branchListCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchListCmd.MarkFlagRequired("cluster")

	branchCreateCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchCreateCmd.Flags().StringVar(&branchParent, "parent", "main", "Parent branch name")
	branchCreateCmd.Flags().BoolVar(&branchEndpoint, "endpoint", false, "Create a compute endpoint for the branch")
	branchCreateCmd.MarkFlagRequired("cluster")

	branchGetCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchGetCmd.MarkFlagRequired("cluster")

	branchDeleteCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchDeleteCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force delete without confirmation")
	branchDeleteCmd.MarkFlagRequired("cluster")

	branchConnectCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchConnectCmd.MarkFlagRequired("cluster")

	branchResetCmd.Flags().StringVar(&branchCluster, "cluster", "", "Cluster name (required)")
	branchResetCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force reset without confirmation")
	branchResetCmd.MarkFlagRequired("cluster")
}

func runBranchList(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	branches, err := client.ListBranches(cmd.Context(), branchCluster)
	if err != nil {
		return fmt.Errorf("failed to list branches: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"branches": branches}, func() {
		output.PrintBranchTable(branches)
	})

	return nil
}

func runBranchCreate(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	branch, err := client.CreateBranch(cmd.Context(), branchCluster, name, branchParent)
	if err != nil {
		return fmt.Errorf("failed to create branch: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(branch)
	} else {
		output.Success("Branch '%s' created from '%s'", name, branchParent)
		if branchEndpoint {
			output.Info("Endpoint is being provisioned. Run 'orochi branch connect %s --cluster %s' to get connection string", name, branchCluster)
		}
	}

	return nil
}

func runBranchGet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	branch, err := client.GetBranch(cmd.Context(), branchCluster, name)
	if err != nil {
		return fmt.Errorf("failed to get branch: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, branch, func() {
		output.PrintBranchDetails(branch)
	})

	return nil
}

func runBranchDelete(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	if !forceDelete {
		output.Warning("You are about to delete branch '%s' from cluster '%s'", name, branchCluster)
		if !output.Confirm("Are you sure?") {
			output.Print("Deletion cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.DeleteBranch(cmd.Context(), branchCluster, name); err != nil {
		return fmt.Errorf("failed to delete branch: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Branch '%s' deleted", name))

	return nil
}

func runBranchConnect(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	client := api.NewClient()
	branch, err := client.GetBranch(cmd.Context(), branchCluster, name)
	if err != nil {
		return fmt.Errorf("failed to get branch: %w", err)
	}

	connStr := branch.ConnectionURI
	if connStr == "" {
		return fmt.Errorf("no connection string available for branch '%s' (branch may not have an endpoint)", name)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]string{
			"cluster":           branchCluster,
			"branch":            name,
			"connection_string": connStr,
		})
	} else {
		output.PrintConnectionString(connStr)
	}

	return nil
}

func runBranchReset(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	if !forceDelete {
		output.Warning("You are about to reset branch '%s' to its parent state", name)
		output.Print("This will discard all changes made to the branch.")
		if !output.Confirm("Are you sure?") {
			output.Print("Reset cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	_, err := client.ResetBranch(cmd.Context(), branchCluster, name, "parent")
	if err != nil {
		return fmt.Errorf("failed to reset branch: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Branch '%s' has been reset", name))

	return nil
}
