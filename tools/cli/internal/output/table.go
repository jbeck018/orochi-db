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
	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
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

// Confirm prompts the user for a yes/no confirmation.
func Confirm(prompt string) bool {
	fmt.Printf("%s [y/N]: ", prompt)
	var response string
	fmt.Scanln(&response)
	response = strings.ToLower(strings.TrimSpace(response))
	return response == "y" || response == "yes"
}

// PrintBackupTable prints a table of backups.
func PrintBackupTable(backups []api.Backup) {
	if len(backups) == 0 {
		Info("No backups found.")
		return
	}

	table := NewTable([]string{"ID", "CLUSTER", "STATUS", "SIZE", "CREATED"})
	for _, b := range backups {
		table.Append([]string{
			b.ID,
			b.ClusterName,
			b.Status,
			b.Size,
			FormatTimeRelative(b.CreatedAt),
		})
	}
	table.Render()
}

// PrintLogs prints log entries.
func PrintLogs(logs []api.LogEntry) {
	if len(logs) == 0 {
		Info("No logs found.")
		return
	}
	for _, log := range logs {
		levelColor := InfoColor
		switch log.Level {
		case "error":
			levelColor = ErrorColor
		case "warning":
			levelColor = WarningColor
		}
		fmt.Printf("[%s] ", FormatTime(log.Timestamp))
		levelColor.Printf("[%s] ", strings.ToUpper(log.Level))
		fmt.Println(log.Message)
	}
}

// PrintEvents prints cluster events.
func PrintEvents(events []api.Event) {
	if len(events) == 0 {
		Info("No events found.")
		return
	}

	table := NewTable([]string{"TIME", "TYPE", "MESSAGE"})
	for _, e := range events {
		table.Append([]string{
			FormatTimeRelative(e.Timestamp),
			e.Type,
			e.Message,
		})
	}
	table.Render()
}

// PrintAuditLogs prints audit log entries.
func PrintAuditLogs(logs []api.AuditEntry) {
	if len(logs) == 0 {
		Info("No audit logs found.")
		return
	}

	table := NewTable([]string{"TIME", "TYPE", "USER", "ACTION"})
	for _, a := range logs {
		table.Append([]string{
			FormatTimeRelative(a.Timestamp),
			a.Type,
			a.User,
			a.Action,
		})
	}
	table.Render()
}

// PrintVersionsTable prints available PostgreSQL versions.
func PrintVersionsTable(versions []api.Version) {
	if len(versions) == 0 {
		Info("No versions available.")
		return
	}

	table := NewTable([]string{"VERSION", "STATUS", "END OF LIFE"})
	for _, v := range versions {
		table.Append([]string{
			v.Version,
			v.Status,
			v.EOL,
		})
	}
	table.Render()
}

// PrintSettingsTable prints cluster settings.
func PrintSettingsTable(settings []api.Setting) {
	if len(settings) == 0 {
		Info("No settings found.")
		return
	}

	table := NewTable([]string{"NAME", "VALUE", "DEFAULT", "RESTART"})
	for _, s := range settings {
		restart := ""
		if s.RequiresRestart {
			restart = "Yes"
		}
		table.Append([]string{
			s.Name,
			s.Value,
			s.DefaultValue,
			restart,
		})
	}
	table.Render()
}

// PrintSettingDetails prints detailed setting information.
func PrintSettingDetails(s *api.Setting) {
	BoldColor.Printf("Setting: %s\n", s.Name)
	fmt.Println(strings.Repeat("-", 40))

	printField("Name", s.Name)
	printField("Value", s.Value)
	printField("Default", s.DefaultValue)
	printField("Type", s.Type)
	if s.RequiresRestart {
		printField("Requires Restart", "Yes")
	}
	if s.Description != "" {
		printField("Description", s.Description)
	}
}

// PrintMetricsSummary prints a metrics summary.
func PrintMetricsSummary(summary *api.MetricsSummary) {
	BoldColor.Println("Metrics Summary")
	fmt.Println(strings.Repeat("-", 40))

	printField("CPU Usage", fmt.Sprintf("%.1f%%", summary.CPUPercent))
	printField("Memory Usage", fmt.Sprintf("%.1f%%", summary.MemoryPercent))
	printField("Storage Used", fmt.Sprintf("%.1f GB / %.1f GB", summary.StorageUsedGB, summary.StorageTotalGB))
	printField("Connections", fmt.Sprintf("%d / %d", summary.ActiveConnections, summary.MaxConnections))
	printField("QPS", fmt.Sprintf("%.1f", summary.QueriesPerSecond))
}

// PrintCPUMetrics prints CPU metrics.
func PrintCPUMetrics(metrics *api.CPUMetrics) {
	BoldColor.Println("CPU Metrics")
	fmt.Println(strings.Repeat("-", 40))

	printField("Current", fmt.Sprintf("%.1f%%", metrics.Current))
	printField("Average", fmt.Sprintf("%.1f%%", metrics.Average))
	printField("Peak", fmt.Sprintf("%.1f%%", metrics.Peak))
}

// PrintMemoryMetrics prints memory metrics.
func PrintMemoryMetrics(metrics *api.MemoryMetrics) {
	BoldColor.Println("Memory Metrics")
	fmt.Println(strings.Repeat("-", 40))

	printField("Used", fmt.Sprintf("%.1f GB", metrics.UsedGB))
	printField("Total", fmt.Sprintf("%.1f GB", metrics.TotalGB))
	printField("Percent", fmt.Sprintf("%.1f%%", metrics.Percent))
}

// PrintStorageMetrics prints storage metrics.
func PrintStorageMetrics(metrics *api.StorageMetrics) {
	BoldColor.Println("Storage Metrics")
	fmt.Println(strings.Repeat("-", 40))

	printField("Used", fmt.Sprintf("%.1f GB", metrics.UsedGB))
	printField("Total", fmt.Sprintf("%.1f GB", metrics.TotalGB))
	printField("Percent", fmt.Sprintf("%.1f%%", metrics.Percent))
}

// PrintQueryMetrics prints query performance metrics.
func PrintQueryMetrics(metrics *api.QueryMetrics) {
	BoldColor.Println("Query Metrics")
	fmt.Println(strings.Repeat("-", 40))

	printField("QPS", fmt.Sprintf("%.1f", metrics.QueriesPerSecond))
	printField("Avg Latency", fmt.Sprintf("%.2f ms", metrics.AvgLatencyMs))
	printField("Slow Queries", fmt.Sprintf("%d", metrics.SlowQueries))
}

// PrintConnectionMetrics prints connection metrics.
func PrintConnectionMetrics(metrics *api.ConnectionMetrics) {
	BoldColor.Println("Connection Metrics")
	fmt.Println(strings.Repeat("-", 40))

	printField("Active", fmt.Sprintf("%d", metrics.Active))
	printField("Idle", fmt.Sprintf("%d", metrics.Idle))
	printField("Max", fmt.Sprintf("%d", metrics.Max))
	printField("Waiting", fmt.Sprintf("%d", metrics.Waiting))
}

// PrintBranchTable prints a table of branches.
func PrintBranchTable(branches []api.Branch) {
	if len(branches) == 0 {
		Info("No branches found.")
		return
	}

	table := NewTable([]string{"NAME", "PARENT", "STATUS", "CREATED"})
	for _, b := range branches {
		table.Append([]string{
			b.Name,
			b.ParentBranch,
			b.Status,
			FormatTimeRelative(b.CreatedAt),
		})
	}
	table.Render()
}

// PrintBranchDetails prints detailed branch information.
func PrintBranchDetails(b *api.Branch) {
	BoldColor.Printf("Branch: %s\n", b.Name)
	fmt.Println(strings.Repeat("-", 40))

	printField("ID", b.ID)
	printField("Name", b.Name)
	printField("Parent", b.ParentBranch)
	printField("Status", b.Status)
	printField("Created", FormatTime(b.CreatedAt))
}

// PrintOrgTable prints a table of organizations.
func PrintOrgTable(orgs []api.Organization) {
	if len(orgs) == 0 {
		Info("No organizations found.")
		return
	}

	table := NewTable([]string{"NAME", "SLUG", "ROLE", "MEMBERS"})
	for _, o := range orgs {
		table.Append([]string{
			o.Name,
			o.Slug,
			o.Role,
			fmt.Sprintf("%d", o.MemberCount),
		})
	}
	table.Render()
}

// PrintOrgDetails prints detailed organization information.
func PrintOrgDetails(o *api.Organization) {
	BoldColor.Printf("Organization: %s\n", o.Name)
	fmt.Println(strings.Repeat("-", 40))

	printField("ID", o.ID)
	printField("Name", o.Name)
	printField("Slug", o.Slug)
	printField("Role", o.Role)
	printField("Members", fmt.Sprintf("%d", o.MemberCount))
	printField("Created", FormatTime(o.CreatedAt))
}

// PrintMembersTable prints organization members.
func PrintMembersTable(members []api.OrgMember) {
	if len(members) == 0 {
		Info("No members found.")
		return
	}

	table := NewTable([]string{"EMAIL", "NAME", "ROLE", "JOINED"})
	for _, m := range members {
		table.Append([]string{
			m.Email,
			m.Name,
			m.Role,
			FormatTimeRelative(m.JoinedAt),
		})
	}
	table.Render()
}
