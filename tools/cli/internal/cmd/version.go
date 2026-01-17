package cmd

import (
	"runtime"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

// Version information - set at build time
var (
	Version   = "1.0.0"
	GitCommit = "unknown"
	BuildDate = "unknown"
)

// VersionInfo contains version details.
type VersionInfo struct {
	Version   string `json:"version"`
	GitCommit string `json:"git_commit"`
	BuildDate string `json:"build_date"`
	GoVersion string `json:"go_version"`
	Platform  string `json:"platform"`
}

var versionCmd = &cobra.Command{
	Use:   "version",
	Short: "Show CLI version",
	Long: `Display version information for the Orochi Cloud CLI.

Examples:
  orochi version
  orochi version --output json`,
	RunE: runVersion,
}

func runVersion(cmd *cobra.Command, args []string) error {
	info := VersionInfo{
		Version:   Version,
		GitCommit: GitCommit,
		BuildDate: BuildDate,
		GoVersion: runtime.Version(),
		Platform:  runtime.GOOS + "/" + runtime.GOARCH,
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(info)
	} else {
		output.Bold("Orochi Cloud CLI")
		output.Print("")
		output.Print("Version:    %s", info.Version)
		output.Print("Git Commit: %s", info.GitCommit)
		output.Print("Built:      %s", info.BuildDate)
		output.Print("Go Version: %s", info.GoVersion)
		output.Print("Platform:   %s", info.Platform)
	}

	return nil
}
