// Package output provides formatting utilities for CLI output.
package output

import (
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/fatih/color"
	"github.com/olekukonko/tablewriter"
	"github.com/orochi-db/orochi-cloud/cli/internal/api"
)

// Colors for output
var (
	SuccessColor = color.New(color.FgGreen)
	ErrorColor   = color.New(color.FgRed)
	WarningColor = color.New(color.FgYellow)
	InfoColor    = color.New(color.FgCyan)
	BoldColor    = color.New(color.Bold)
	DimColor     = color.New(color.Faint)
)

// StatusColors maps cluster statuses to colors
var StatusColors = map[api.ClusterStatus]*color.Color{
	api.ClusterStatusCreating:    color.New(color.FgYellow),
	api.ClusterStatusRunning:     color.New(color.FgGreen),
	api.ClusterStatusStopped:     color.New(color.FgRed),
	api.ClusterStatusDeleting:    color.New(color.FgRed),
	api.ClusterStatusScaling:     color.New(color.FgYellow),
	api.ClusterStatusMaintenance: color.New(color.FgYellow),
	api.ClusterStatusError:       color.New(color.FgRed, color.Bold),
}

// Success prints a success message.
func Success(format string, args ...interface{}) {
	SuccessColor.Printf("SUCCESS: "+format+"\n", args...)
}

// Error prints an error message.
func Error(format string, args ...interface{}) {
	ErrorColor.Fprintf(os.Stderr, "ERROR: "+format+"\n", args...)
}

// Warning prints a warning message.
func Warning(format string, args ...interface{}) {
	WarningColor.Printf("WARNING: "+format+"\n", args...)
}

// Info prints an info message.
func Info(format string, args ...interface{}) {
	InfoColor.Printf(format+"\n", args...)
}

// Print prints a plain message.
func Print(format string, args ...interface{}) {
	fmt.Printf(format+"\n", args...)
}

// Bold prints bold text.
func Bold(format string, args ...interface{}) {
	BoldColor.Printf(format+"\n", args...)
}

// Dim prints dimmed text.
func Dim(format string, args ...interface{}) {
	DimColor.Printf(format+"\n", args...)
}

// NewTable creates a new table writer.
func NewTable(headers []string) *tablewriter.Table {
	return NewTableWithWriter(os.Stdout, headers)
}

// NewTableWithWriter creates a new table writer with a custom writer.
func NewTableWithWriter(w io.Writer, headers []string) *tablewriter.Table {
	table := tablewriter.NewWriter(w)
	table.SetHeader(headers)
	table.SetBorder(false)
	table.SetHeaderAlignment(tablewriter.ALIGN_LEFT)
	table.SetAlignment(tablewriter.ALIGN_LEFT)
	table.SetCenterSeparator("")
	table.SetColumnSeparator("")
	table.SetRowSeparator("")
	table.SetHeaderLine(false)
	table.SetTablePadding("  ")
	table.SetNoWhiteSpace(true)
	return table
}

// FormatStatus returns a colored status string.
func FormatStatus(status api.ClusterStatus) string {
	c, ok := StatusColors[status]
	if !ok {
		return string(status)
	}
	return c.Sprint(strings.ToUpper(string(status)))
}

// FormatTime formats a time for display.
func FormatTime(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.Format("2006-01-02 15:04:05")
}

// FormatTimeRelative formats a time as relative duration.
func FormatTimeRelative(t time.Time) string {
	if t.IsZero() {
		return "-"
	}

	duration := time.Since(t)

	switch {
	case duration < time.Minute:
		return "just now"
	case duration < time.Hour:
		mins := int(duration.Minutes())
		if mins == 1 {
			return "1 minute ago"
		}
		return fmt.Sprintf("%d minutes ago", mins)
	case duration < 24*time.Hour:
		hours := int(duration.Hours())
		if hours == 1 {
			return "1 hour ago"
		}
		return fmt.Sprintf("%d hours ago", hours)
	case duration < 30*24*time.Hour:
		days := int(duration.Hours() / 24)
		if days == 1 {
			return "1 day ago"
		}
		return fmt.Sprintf("%d days ago", days)
	default:
		return t.Format("Jan 2, 2006")
	}
}

// PrintClusterTable prints a table of clusters.
func PrintClusterTable(clusters []api.Cluster) {
	if len(clusters) == 0 {
		Info("No clusters found.")
		return
	}

	table := NewTable([]string{"NAME", "SIZE", "REGION", "REPLICAS", "STATUS", "CREATED"})

	for _, c := range clusters {
		table.Append([]string{
			c.Name,
			string(c.Size),
			c.Region,
			fmt.Sprintf("%d", c.Replicas),
			FormatStatus(c.Status),
			FormatTimeRelative(c.CreatedAt),
		})
	}

	table.Render()
}

// PrintClusterDetails prints detailed information about a cluster.
func PrintClusterDetails(c *api.Cluster) {
	BoldColor.Printf("Cluster: %s\n", c.Name)
	fmt.Println(strings.Repeat("-", 40))

	printField("ID", c.ID)
	printField("Name", c.Name)
	printField("Size", string(c.Size))
	printField("Region", c.Region)
	printField("Replicas", fmt.Sprintf("%d", c.Replicas))
	printField("Status", FormatStatus(c.Status))
	printField("Version", c.Version)
	printField("vCPU", fmt.Sprintf("%d", c.VCPU))
	printField("Memory", fmt.Sprintf("%d GB", c.MemoryGB))
	printField("Storage", fmt.Sprintf("%d GB", c.StorageGB))

	if c.Host != "" {
		printField("Host", c.Host)
		printField("Port", fmt.Sprintf("%d", c.Port))
	}

	printField("Created", FormatTime(c.CreatedAt))
	printField("Updated", FormatTime(c.UpdatedAt))
}

// printField prints a labeled field.
func printField(label, value string) {
	DimColor.Printf("%-12s", label+":")
	fmt.Printf(" %s\n", value)
}

// PrintUserDetails prints user information.
func PrintUserDetails(u *api.User) {
	BoldColor.Println("Current User")
	fmt.Println(strings.Repeat("-", 40))

	printField("ID", u.ID)
	printField("Email", u.Email)
	printField("Name", u.Name)
	printField("Member Since", FormatTime(u.CreatedAt))
}

// PrintConfigTable prints configuration key-value pairs.
func PrintConfigTable(config map[string]interface{}) {
	if len(config) == 0 {
		Info("No configuration found.")
		return
	}

	table := NewTable([]string{"KEY", "VALUE"})

	for key, value := range config {
		// Mask sensitive values
		strVal := fmt.Sprintf("%v", value)
		if strings.Contains(key, "token") || strings.Contains(key, "password") || strings.Contains(key, "secret") {
			if strVal != "" {
				strVal = maskValue(strVal)
			}
		}
		table.Append([]string{key, strVal})
	}

	table.Render()
}

// maskValue masks a sensitive value for display.
func maskValue(value string) string {
	if len(value) <= 8 {
		return strings.Repeat("*", len(value))
	}
	return value[:4] + strings.Repeat("*", len(value)-8) + value[len(value)-4:]
}

// PrintConnectionString prints the connection string in a copyable format.
func PrintConnectionString(connStr string) {
	BoldColor.Println("Connection String")
	fmt.Println(strings.Repeat("-", 40))
	fmt.Println()
	InfoColor.Println(connStr)
	fmt.Println()
	Dim("Copy the connection string above to connect to your cluster.")
}

// Spinner provides a simple spinner for long operations.
type Spinner struct {
	message  string
	done     chan struct{}
	stopOnce sync.Once
}

// NewSpinner creates a new spinner.
func NewSpinner(message string) *Spinner {
	return &Spinner{
		message: message,
		done:    make(chan struct{}),
	}
}

// Start starts the spinner.
func (s *Spinner) Start() {
	frames := []string{"|", "/", "-", "\\"}
	go func() {
		i := 0
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-s.done:
				fmt.Print("\r" + strings.Repeat(" ", len(s.message)+10) + "\r")
				return
			case <-ticker.C:
				fmt.Printf("\r%s %s", frames[i%len(frames)], s.message)
				i++
			}
		}
	}()
}

// Stop stops the spinner. Safe to call multiple times.
func (s *Spinner) Stop() {
	s.stopOnce.Do(func() {
		close(s.done)
	})
}
