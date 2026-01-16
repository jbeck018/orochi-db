package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-cloud/cli/internal/api"
	"github.com/orochi-db/orochi-cloud/cli/internal/config"
	"github.com/orochi-db/orochi-cloud/cli/internal/output"
)

var logoutCmd = &cobra.Command{
	Use:   "logout",
	Short: "Logout from Orochi Cloud",
	Long: `Logout from Orochi Cloud and clear stored credentials.

This will:
- Invalidate your session on the server
- Remove stored credentials from ~/.orochi/config.yaml

Example:
  orochi logout`,
	RunE: runLogout,
}

func runLogout(cmd *cobra.Command, args []string) error {
	if !config.IsAuthenticated() {
		output.Info("You are not logged in.")
		return nil
	}

	// Attempt to invalidate session on server
	client := api.NewClient()
	if err := client.Logout(); err != nil {
		// Log but don't fail - we still want to clear local credentials
		if verbose {
			output.Warning("Failed to invalidate server session: %v", err)
		}
	}

	// Clear local credentials
	if err := config.ClearCredentials(); err != nil {
		return fmt.Errorf("failed to clear credentials: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, "Successfully logged out")

	return nil
}

var whoamiCmd = &cobra.Command{
	Use:   "whoami",
	Short: "Show current user",
	Long: `Display information about the currently logged in user.

This command shows:
- User ID
- Email address
- Display name
- Account creation date

Example:
  orochi whoami
  orochi whoami --output json`,
	RunE: runWhoami,
}

func runWhoami(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	user, err := client.GetCurrentUser()
	if err != nil {
		return fmt.Errorf("failed to get user info: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, user, func() {
		output.PrintUserDetails(user)
	})

	return nil
}
