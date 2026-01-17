// Package main is the entry point for the Orochi Cloud CLI.
package main

import (
	"os"

	"github.com/orochi-db/orochi-db/tools/cli/internal/cmd"
)

func main() {
	if err := cmd.Execute(); err != nil {
		os.Exit(1)
	}
}
