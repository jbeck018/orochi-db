// Package cmd provides the CLI commands for Orochi Cloud.
package cmd

import (
	"os"

	"github.com/spf13/cobra"
	"github.com/orochi-db/orochi-cloud/cli/internal/config"
	"github.com/orochi-db/orochi-cloud/cli/internal/output"
)

var (
	// Global flags
	outputFormat string
	verbose      bool
)

// rootCmd represents the base command when called without any subcommands.
var rootCmd = &cobra.Command{
	Use:   "orochi",
	Short: "Orochi Cloud CLI - Manage your Orochi Cloud clusters",
	Long: `Orochi Cloud CLI is a command-line interface for managing
your Orochi Cloud database clusters.

Get started by logging in:
  orochi login

Then create your first cluster:
  orochi cluster create my-cluster --size small --region us-east-1

For more information, visit: https://docs.orochi.cloud`,
	SilenceUsage:  true,
	SilenceErrors: true,
	PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
		// Initialize configuration
		if err := config.Init(); err != nil {
			return err
		}

		// Override output format if flag is set
		if outputFormat == "" {
			outputFormat = config.GetOutputFormat()
		}

		return nil
	},
}

// Execute runs the root command.
func Execute() error {
	if err := rootCmd.Execute(); err != nil {
		output.Error("%v", err)
		return err
	}
	return nil
}

func init() {
	// Global flags
	rootCmd.PersistentFlags().StringVarP(&outputFormat, "output", "o", "", "Output format (table, json)")
	rootCmd.PersistentFlags().BoolVarP(&verbose, "verbose", "v", false, "Enable verbose output")

	// Add subcommands
	rootCmd.AddCommand(loginCmd)
	rootCmd.AddCommand(logoutCmd)
	rootCmd.AddCommand(whoamiCmd)
	rootCmd.AddCommand(clusterCmd)
	rootCmd.AddCommand(configCmd)
	rootCmd.AddCommand(versionCmd)
}

// getOutputFormat returns the current output format.
func getOutputFormat() string {
	if outputFormat != "" {
		return outputFormat
	}
	return config.GetOutputFormat()
}

// requireAuth checks if the user is authenticated and returns an error if not.
func requireAuth() error {
	if !config.IsAuthenticated() {
		return &AuthError{Message: "Not logged in. Please run 'orochi login' first."}
	}
	return nil
}

// AuthError represents an authentication error.
type AuthError struct {
	Message string
}

func (e *AuthError) Error() string {
	return e.Message
}

// ExitIfError exits with code 1 if err is not nil.
func ExitIfError(err error) {
	if err != nil {
		output.Error("%v", err)
		os.Exit(1)
	}
}
