package cmd

import (
	"fmt"
	"strings"

	"github.com/fatih/color"
	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	clientsDatabase string
	clientsUser     string
	clientsState    string
)

var poolerClientsCmd = &cobra.Command{
	Use:   "clients <cluster-id>",
	Short: "List connected clients",
	Long: `List all clients currently connected to the PgDog connection pooler.

Displays information about each connected client including:
- Client address and port
- Database and user
- Connection state (active, idle, waiting)
- Application name
- Connection duration
- Query statistics

Client States:
  active  - Currently executing a query
  idle    - Connected but not executing
  waiting - Waiting for a backend connection

Examples:
  # List all connected clients
  orochi cluster pooler clients my-cluster

  # Filter by database
  orochi cluster pooler clients my-cluster --database mydb

  # Filter by user
  orochi cluster pooler clients my-cluster --user admin

  # Filter by state
  orochi cluster pooler clients my-cluster --state active

  # Combine filters
  orochi cluster pooler clients my-cluster --database mydb --user app_user

  # Output as JSON
  orochi cluster pooler clients my-cluster --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runPoolerClients,
}

func init() {
	poolerClientsCmd.Flags().StringVarP(&clientsDatabase, "database", "d", "", "Filter by database name")
	poolerClientsCmd.Flags().StringVarP(&clientsUser, "user", "u", "", "Filter by username")
	poolerClientsCmd.Flags().StringVarP(&clientsState, "state", "s", "", "Filter by state (active, idle, waiting)")
}

func runPoolerClients(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	clusterID := args[0]

	// Validate state filter if provided
	if clientsState != "" {
		validStates := map[string]bool{"active": true, "idle": true, "waiting": true}
		if !validStates[strings.ToLower(clientsState)] {
			return fmt.Errorf("invalid state '%s': must be 'active', 'idle', or 'waiting'", clientsState)
		}
		clientsState = strings.ToLower(clientsState)
	}

	// Build filter
	var filter *api.PoolerClientFilter
	if clientsDatabase != "" || clientsUser != "" || clientsState != "" {
		filter = &api.PoolerClientFilter{
			Database: clientsDatabase,
			User:     clientsUser,
			State:    clientsState,
		}
	}

	client := api.NewClient()
	clients, err := client.GetPoolerClients(cmd.Context(), clusterID, filter)
	if err != nil {
		return fmt.Errorf("failed to get pooler clients: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"clients": clients}, func() {
		printPoolerClients(clusterID, clients, filter)
	})

	return nil
}

func printPoolerClients(clusterID string, clients []api.PoolerClient, filter *api.PoolerClientFilter) {
	output.BoldColor.Printf("Connected Clients: %s\n", clusterID)
	fmt.Println(strings.Repeat("=", 80))

	// Show active filters
	if filter != nil {
		filterParts := []string{}
		if filter.Database != "" {
			filterParts = append(filterParts, fmt.Sprintf("database=%s", filter.Database))
		}
		if filter.User != "" {
			filterParts = append(filterParts, fmt.Sprintf("user=%s", filter.User))
		}
		if filter.State != "" {
			filterParts = append(filterParts, fmt.Sprintf("state=%s", filter.State))
		}
		if len(filterParts) > 0 {
			output.DimColor.Printf("Filters: %s\n", strings.Join(filterParts, ", "))
		}
	}
	fmt.Println()

	if len(clients) == 0 {
		output.Info("No connected clients found.")
		return
	}

	// Summary stats
	activeCount := 0
	idleCount := 0
	waitingCount := 0
	for _, c := range clients {
		switch strings.ToLower(c.State) {
		case "active":
			activeCount++
		case "idle":
			idleCount++
		case "waiting":
			waitingCount++
		}
	}

	fmt.Printf("Total: %d clients (", len(clients))
	color.New(color.FgGreen).Printf("%d active", activeCount)
	fmt.Printf(", ")
	color.New(color.FgYellow).Printf("%d idle", idleCount)
	fmt.Printf(", ")
	color.New(color.FgCyan).Printf("%d waiting", waitingCount)
	fmt.Printf(")\n\n")

	// Client table
	table := output.NewTable([]string{"ADDRESS", "DATABASE", "USER", "STATE", "APP NAME", "CONNECTED", "QUERIES"})

	for _, c := range clients {
		address := fmt.Sprintf("%s:%d", c.Address, c.Port)
		state := formatClientState(c.State)
		appName := c.ApplicationName
		if appName == "" {
			appName = "-"
		}
		if len(appName) > 20 {
			appName = appName[:17] + "..."
		}

		table.Append([]string{
			address,
			c.Database,
			c.User,
			state,
			appName,
			output.FormatTimeRelative(c.ConnectedAt),
			formatLargeNumber(c.TotalQueries),
		})
	}

	table.Render()

	// Show details for active clients
	activeClients := []api.PoolerClient{}
	for _, c := range clients {
		if strings.ToLower(c.State) == "active" && c.CurrentQuery != "" {
			activeClients = append(activeClients, c)
		}
	}

	if len(activeClients) > 0 {
		fmt.Println()
		output.BoldColor.Println("Active Queries")
		fmt.Println(strings.Repeat("-", 80))

		for _, c := range activeClients {
			fmt.Printf("\n")
			output.DimColor.Printf("Client: ")
			fmt.Printf("%s:%d (%s@%s)\n", c.Address, c.Port, c.User, c.Database)

			output.DimColor.Printf("Duration: ")
			fmt.Printf("%.2f ms\n", c.QueryDuration)

			output.DimColor.Printf("Query: ")
			query := c.CurrentQuery
			if len(query) > 200 {
				query = query[:197] + "..."
			}
			output.InfoColor.Printf("%s\n", query)
		}
	}
}

func formatClientState(state string) string {
	switch strings.ToLower(state) {
	case "active":
		return color.New(color.FgGreen).Sprint("active")
	case "idle":
		return color.New(color.FgYellow).Sprint("idle")
	case "waiting":
		return color.New(color.FgCyan).Sprint("waiting")
	default:
		return state
	}
}
