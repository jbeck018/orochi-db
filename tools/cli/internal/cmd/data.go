package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	dataCluster    string
	dataDatabase   string
	dataTable      string
	dataFormat     string
	dataFile       string
	dataURL        string
	dataCompressed bool
	dataDelimiter  string
	dataHeader     bool
)

var dataCmd = &cobra.Command{
	Use:   "data",
	Short: "Import and export data",
	Long: `Import and export data to/from your Orochi Cloud clusters.

Commands:
  import    Import data from a file or URL
  export    Export data to a file
  copy      Copy data between clusters

Examples:
  orochi data import --cluster my-cluster --file data.csv --table users
  orochi data export --cluster my-cluster --table users --file users.csv
  orochi data copy --from prod-cluster --to dev-cluster --table users`,
}

var dataImportCmd = &cobra.Command{
	Use:   "import",
	Short: "Import data from a file or URL",
	Long: `Import data into a table from a file or URL.

Supported formats: csv, json, jsonl, parquet

Examples:
  orochi data import --cluster my-cluster --file data.csv --table users
  orochi data import --cluster my-cluster --url https://example.com/data.csv --table users
  orochi data import --cluster my-cluster --file data.json --table users --format json
  orochi data import --cluster my-cluster --file data.parquet --table events --format parquet`,
	RunE: runDataImport,
}

var dataExportCmd = &cobra.Command{
	Use:   "export",
	Short: "Export data to a file",
	Long: `Export table data to a file.

Supported formats: csv, json, jsonl, parquet

Examples:
  orochi data export --cluster my-cluster --table users --file users.csv
  orochi data export --cluster my-cluster --table users --file users.json --format json
  orochi data export --cluster my-cluster --database analytics --table events --file events.parquet --format parquet`,
	RunE: runDataExport,
}

var dataCopyCmd = &cobra.Command{
	Use:   "copy",
	Short: "Copy data between clusters",
	Long: `Copy table data from one cluster to another.

Examples:
  orochi data copy --from prod-cluster --to dev-cluster --table users
  orochi data copy --from prod-cluster --to staging-cluster --database mydb --table orders`,
	RunE: runDataCopy,
}

var (
	dataFromCluster string
	dataToCluster   string
)

func init() {
	// Add subcommands
	dataCmd.AddCommand(dataImportCmd)
	dataCmd.AddCommand(dataExportCmd)
	dataCmd.AddCommand(dataCopyCmd)

	// Import flags
	dataImportCmd.Flags().StringVar(&dataCluster, "cluster", "", "Target cluster name (required)")
	dataImportCmd.Flags().StringVar(&dataDatabase, "database", "postgres", "Target database name")
	dataImportCmd.Flags().StringVar(&dataTable, "table", "", "Target table name (required)")
	dataImportCmd.Flags().StringVar(&dataFile, "file", "", "Local file path to import")
	dataImportCmd.Flags().StringVar(&dataURL, "url", "", "URL to import data from")
	dataImportCmd.Flags().StringVar(&dataFormat, "format", "csv", "Data format (csv, json, jsonl, parquet)")
	dataImportCmd.Flags().BoolVar(&dataCompressed, "compressed", false, "File is gzip compressed")
	dataImportCmd.Flags().StringVar(&dataDelimiter, "delimiter", ",", "CSV delimiter character")
	dataImportCmd.Flags().BoolVar(&dataHeader, "header", true, "CSV has header row")
	dataImportCmd.MarkFlagRequired("cluster")
	dataImportCmd.MarkFlagRequired("table")

	// Export flags
	dataExportCmd.Flags().StringVar(&dataCluster, "cluster", "", "Source cluster name (required)")
	dataExportCmd.Flags().StringVar(&dataDatabase, "database", "postgres", "Source database name")
	dataExportCmd.Flags().StringVar(&dataTable, "table", "", "Source table name (required)")
	dataExportCmd.Flags().StringVar(&dataFile, "file", "", "Output file path (required)")
	dataExportCmd.Flags().StringVar(&dataFormat, "format", "csv", "Output format (csv, json, jsonl, parquet)")
	dataExportCmd.Flags().BoolVar(&dataCompressed, "compressed", false, "Compress output with gzip")
	dataExportCmd.Flags().StringVar(&dataDelimiter, "delimiter", ",", "CSV delimiter character")
	dataExportCmd.Flags().BoolVar(&dataHeader, "header", true, "Include CSV header row")
	dataExportCmd.MarkFlagRequired("cluster")
	dataExportCmd.MarkFlagRequired("table")
	dataExportCmd.MarkFlagRequired("file")

	// Copy flags
	dataCopyCmd.Flags().StringVar(&dataFromCluster, "from", "", "Source cluster name (required)")
	dataCopyCmd.Flags().StringVar(&dataToCluster, "to", "", "Target cluster name (required)")
	dataCopyCmd.Flags().StringVar(&dataDatabase, "database", "postgres", "Database name")
	dataCopyCmd.Flags().StringVar(&dataTable, "table", "", "Table name (required)")
	dataCopyCmd.MarkFlagRequired("from")
	dataCopyCmd.MarkFlagRequired("to")
	dataCopyCmd.MarkFlagRequired("table")
}

func runDataImport(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	// Validate input source
	if dataFile == "" && dataURL == "" {
		return fmt.Errorf("either --file or --url must be specified")
	}
	if dataFile != "" && dataURL != "" {
		return fmt.Errorf("only one of --file or --url can be specified")
	}

	// Validate format
	validFormats := map[string]bool{"csv": true, "json": true, "jsonl": true, "parquet": true}
	if !validFormats[dataFormat] {
		return fmt.Errorf("invalid format: %s (must be csv, json, jsonl, or parquet)", dataFormat)
	}

	client := api.NewClient()

	var job *api.DataJob
	var err error

	if dataFile != "" {
		output.Info("Uploading and importing '%s' to %s.%s...", dataFile, dataDatabase, dataTable)
		job, err = client.ImportFromFile(cmd.Context(), &api.ImportRequest{
			Cluster:    dataCluster,
			Database:   dataDatabase,
			Table:      dataTable,
			FilePath:   dataFile,
			Format:     dataFormat,
			Compressed: dataCompressed,
			Delimiter:  dataDelimiter,
			HasHeader:  dataHeader,
		})
	} else {
		output.Info("Importing from '%s' to %s.%s...", dataURL, dataDatabase, dataTable)
		job, err = client.ImportFromURL(cmd.Context(), &api.ImportRequest{
			Cluster:    dataCluster,
			Database:   dataDatabase,
			Table:      dataTable,
			URL:        dataURL,
			Format:     dataFormat,
			Compressed: dataCompressed,
			Delimiter:  dataDelimiter,
			HasHeader:  dataHeader,
		})
	}

	if err != nil {
		return fmt.Errorf("failed to start import: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(job)
	} else {
		output.Success("Import job '%s' started", job.ID)
		output.Info("Run 'orochi data status %s' to check progress", job.ID)
	}

	return nil
}

func runDataExport(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	// Validate format
	validFormats := map[string]bool{"csv": true, "json": true, "jsonl": true, "parquet": true}
	if !validFormats[dataFormat] {
		return fmt.Errorf("invalid format: %s (must be csv, json, jsonl, or parquet)", dataFormat)
	}

	output.Info("Exporting %s.%s from '%s'...", dataDatabase, dataTable, dataCluster)

	client := api.NewClient()
	job, err := client.ExportToFile(cmd.Context(), &api.ExportRequest{
		Cluster:    dataCluster,
		Database:   dataDatabase,
		Table:      dataTable,
		FilePath:   dataFile,
		Format:     dataFormat,
		Compressed: dataCompressed,
		Delimiter:  dataDelimiter,
		HasHeader:  dataHeader,
	})
	if err != nil {
		return fmt.Errorf("failed to start export: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(job)
	} else {
		output.Success("Export job '%s' started", job.ID)
		output.Info("Output will be saved to: %s", dataFile)
	}

	return nil
}

func runDataCopy(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	output.Info("Copying %s.%s from '%s' to '%s'...", dataDatabase, dataTable, dataFromCluster, dataToCluster)

	client := api.NewClient()
	job, err := client.CopyData(cmd.Context(), &api.CopyRequest{
		FromCluster: dataFromCluster,
		ToCluster:   dataToCluster,
		Database:    dataDatabase,
		Table:       dataTable,
	})
	if err != nil {
		return fmt.Errorf("failed to start copy: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(job)
	} else {
		output.Success("Copy job '%s' started", job.ID)
		output.Info("Run 'orochi data status %s' to check progress", job.ID)
	}

	return nil
}
