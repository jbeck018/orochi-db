package cmd

import (
	"bufio"
	"fmt"
	"os"
	"strings"
	"syscall"

	"github.com/spf13/cobra"
	"golang.org/x/term"

	"github.com/orochi-db/orochi-cloud/cli/internal/api"
	"github.com/orochi-db/orochi-cloud/cli/internal/config"
	"github.com/orochi-db/orochi-cloud/cli/internal/output"
)

var loginCmd = &cobra.Command{
	Use:   "login",
	Short: "Login to Orochi Cloud",
	Long: `Login to Orochi Cloud using your email and password.

Your credentials will be securely stored in ~/.orochi/config.yaml.

Example:
  orochi login`,
	RunE: runLogin,
}

func runLogin(cmd *cobra.Command, args []string) error {
	// Check if already logged in
	if config.IsAuthenticated() {
		output.Warning("You are already logged in. Run 'orochi logout' first to login as a different user.")

		reader := bufio.NewReader(os.Stdin)
		fmt.Print("Continue anyway? [y/N]: ")
		response, err := reader.ReadString('\n')
		if err != nil {
			return fmt.Errorf("failed to read response: %w", err)
		}

		response = strings.TrimSpace(strings.ToLower(response))
		if response != "y" && response != "yes" {
			output.Info("Login cancelled.")
			return nil
		}
	}

	// Prompt for email
	reader := bufio.NewReader(os.Stdin)
	fmt.Print("Email: ")
	email, err := reader.ReadString('\n')
	if err != nil {
		return fmt.Errorf("failed to read email: %w", err)
	}
	email = strings.TrimSpace(email)

	if email == "" {
		return fmt.Errorf("email cannot be empty")
	}

	// Prompt for password (hidden input)
	fmt.Print("Password: ")
	passwordBytes, err := term.ReadPassword(int(syscall.Stdin))
	if err != nil {
		return fmt.Errorf("failed to read password: %w", err)
	}
	fmt.Println() // Add newline after hidden input

	password := string(passwordBytes)
	if password == "" {
		return fmt.Errorf("password cannot be empty")
	}

	// Login via API
	client := api.NewClient()
	loginResp, err := client.Login(email, password)
	if err != nil {
		return fmt.Errorf("login failed: %w", err)
	}

	// Store credentials
	if err := config.SetCredentials(
		loginResp.AccessToken,
		loginResp.RefreshToken,
		loginResp.User.Email,
		loginResp.User.ID,
	); err != nil {
		return fmt.Errorf("failed to store credentials: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]interface{}{
			"status": "success",
			"user": map[string]string{
				"id":    loginResp.User.ID,
				"email": loginResp.User.Email,
				"name":  loginResp.User.Name,
			},
		})
	} else {
		output.Success("Logged in as %s", loginResp.User.Email)
	}

	return nil
}
