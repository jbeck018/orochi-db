package cmd

import (
	"fmt"
	"sort"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/config"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

// Valid configuration keys
var validConfigKeys = map[string]string{
	"api_endpoint":  "API endpoint URL",
	"output_format": "Default output format (table, json)",
}

var configCmd = &cobra.Command{
	Use:   "config",
	Short: "Manage CLI configuration",
	Long: `Manage the Orochi Cloud CLI configuration.

Configuration is stored in ~/.orochi/config.yaml

Available configuration keys:
  api_endpoint   - API endpoint URL (default: https://api.orochi.cloud)
  output_format  - Default output format: table or json (default: table)

Examples:
  orochi config list
  orochi config get api_endpoint
  orochi config set output_format json`,
}

var configSetCmd = &cobra.Command{
	Use:   "set <key> <value>",
	Short: "Set a configuration value",
	Long: `Set a configuration value.

Available keys:
  api_endpoint   - API endpoint URL
  output_format  - Default output format (table, json)

Examples:
  orochi config set api_endpoint https://api.orochi.cloud
  orochi config set output_format json`,
	Args: cobra.ExactArgs(2),
	RunE: runConfigSet,
}

var configGetCmd = &cobra.Command{
	Use:   "get <key>",
	Short: "Get a configuration value",
	Long: `Get a configuration value.

Examples:
  orochi config get api_endpoint
  orochi config get output_format`,
	Args: cobra.ExactArgs(1),
	RunE: runConfigGet,
}

var configListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all configuration values",
	Long: `List all configuration values.

Note: Sensitive values like tokens are masked for security.

Examples:
  orochi config list
  orochi config list --output json`,
	RunE: runConfigList,
}

func init() {
	configCmd.AddCommand(configSetCmd)
	configCmd.AddCommand(configGetCmd)
	configCmd.AddCommand(configListCmd)
}

func runConfigSet(cmd *cobra.Command, args []string) error {
	key := args[0]
	value := args[1]

	// Validate key (allow unknown keys but warn)
	if _, ok := validConfigKeys[key]; !ok {
		// Check if it's a protected key
		if strings.Contains(key, "token") || strings.Contains(key, "password") {
			return fmt.Errorf("cannot set '%s' directly. Use 'orochi login' to set credentials", key)
		}
		output.Warning("Unknown configuration key: %s", key)
	}

	// Validate specific keys
	switch key {
	case "output_format":
		if value != "table" && value != "json" {
			return fmt.Errorf("output_format must be 'table' or 'json', got '%s'", value)
		}
	case "api_endpoint":
		if !strings.HasPrefix(value, "http://") && !strings.HasPrefix(value, "https://") {
			return fmt.Errorf("api_endpoint must start with http:// or https://")
		}
	}

	if err := config.Set(key, value); err != nil {
		return fmt.Errorf("failed to set configuration: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]string{
			"status": "success",
			"key":    key,
			"value":  value,
		})
	} else {
		output.Success("Set %s = %s", key, value)
	}

	return nil
}

func runConfigGet(cmd *cobra.Command, args []string) error {
	key := args[0]

	value := config.Get(key)

	// Mask sensitive values
	if strings.Contains(key, "token") || strings.Contains(key, "password") || strings.Contains(key, "secret") {
		if value != "" {
			value = maskValue(value)
		}
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(map[string]string{
			"key":   key,
			"value": value,
		})
	} else {
		if value == "" {
			output.Info("%s is not set", key)
		} else {
			output.Print("%s = %s", key, value)
		}
	}

	return nil
}

func runConfigList(cmd *cobra.Command, args []string) error {
	allConfig := config.GetAll()

	format := getOutputFormat()
	if format == "json" {
		// Mask sensitive values for JSON output too
		maskedConfig := make(map[string]interface{})
		for k, v := range allConfig {
			strVal := fmt.Sprintf("%v", v)
			if strings.Contains(k, "token") || strings.Contains(k, "password") || strings.Contains(k, "secret") {
				if strVal != "" {
					maskedConfig[k] = maskValue(strVal)
				} else {
					maskedConfig[k] = ""
				}
			} else {
				maskedConfig[k] = v
			}
		}
		output.PrintJSON(maskedConfig)
	} else {
		if len(allConfig) == 0 {
			output.Info("No configuration set.")
			output.Print("")
			output.Print("Available configuration keys:")
			keys := make([]string, 0, len(validConfigKeys))
			for k := range validConfigKeys {
				keys = append(keys, k)
			}
			sort.Strings(keys)
			for _, k := range keys {
				output.Print("  %-15s - %s", k, validConfigKeys[k])
			}
			return nil
		}
		output.PrintConfigTable(allConfig)
	}

	return nil
}

// maskValue masks a sensitive value for display.
func maskValue(value string) string {
	if len(value) <= 8 {
		return strings.Repeat("*", len(value))
	}
	return value[:4] + strings.Repeat("*", len(value)-8) + value[len(value)-4:]
}
