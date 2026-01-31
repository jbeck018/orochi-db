package cmd

import (
	"fmt"
	"regexp"
	"strings"

	"github.com/spf13/cobra"

	"github.com/orochi-db/orochi-db/tools/cli/internal/api"
	"github.com/orochi-db/orochi-db/tools/cli/internal/config"
	"github.com/orochi-db/orochi-db/tools/cli/internal/output"
)

var (
	orgRole string
)

var orgCmd = &cobra.Command{
	Use:   "org",
	Short: "Manage organizations",
	Long: `Manage organizations and team members.

Commands:
  list        List organizations you belong to
  create      Create a new organization
  get         Get organization details
  delete      Delete an organization
  switch      Switch active organization
  members     List organization members
  invite      Invite a user to the organization
  remove      Remove a member from the organization

Examples:
  orochi org list
  orochi org create my-company
  orochi org switch my-company
  orochi org members
  orochi org invite user@example.com --role admin`,
}

var orgListCmd = &cobra.Command{
	Use:   "list",
	Short: "List organizations",
	Long: `List all organizations you are a member of.

Examples:
  orochi org list
  orochi org list --output json`,
	RunE: runOrgList,
}

var orgCreateCmd = &cobra.Command{
	Use:   "create <name>",
	Short: "Create a new organization",
	Long: `Create a new organization.

Examples:
  orochi org create my-company
  orochi org create my-team --output json`,
	Args: cobra.ExactArgs(1),
	RunE: runOrgCreate,
}

var orgGetCmd = &cobra.Command{
	Use:   "get [name]",
	Short: "Get organization details",
	Long: `Get details about an organization. If no name is provided, shows the current organization.

Examples:
  orochi org get
  orochi org get my-company
  orochi org get my-company --output json`,
	Args: cobra.MaximumNArgs(1),
	RunE: runOrgGet,
}

var orgDeleteCmd = &cobra.Command{
	Use:   "delete <name>",
	Short: "Delete an organization",
	Long: `Delete an organization. You must be the owner.

WARNING: This will delete all clusters and data in the organization.

Examples:
  orochi org delete my-company
  orochi org delete my-company --force`,
	Args: cobra.ExactArgs(1),
	RunE: runOrgDelete,
}

var orgSwitchCmd = &cobra.Command{
	Use:   "switch <name>",
	Short: "Switch active organization",
	Long: `Switch to a different organization context.

All subsequent commands will operate in this organization's context.

Examples:
  orochi org switch my-company
  orochi org switch personal`,
	Args: cobra.ExactArgs(1),
	RunE: runOrgSwitch,
}

var orgMembersCmd = &cobra.Command{
	Use:   "members [org-name]",
	Short: "List organization members",
	Long: `List all members of an organization.

Examples:
  orochi org members
  orochi org members my-company
  orochi org members --output json`,
	Args: cobra.MaximumNArgs(1),
	RunE: runOrgMembers,
}

var orgInviteCmd = &cobra.Command{
	Use:   "invite <email>",
	Short: "Invite a user to the organization",
	Long: `Invite a user to join the organization.

Roles:
  owner   - Full access including org deletion
  admin   - Full access except org deletion
  member  - Can manage clusters
  viewer  - Read-only access

Examples:
  orochi org invite user@example.com
  orochi org invite user@example.com --role admin`,
	Args: cobra.ExactArgs(1),
	RunE: runOrgInvite,
}

var orgRemoveCmd = &cobra.Command{
	Use:   "remove <email>",
	Short: "Remove a member from the organization",
	Long: `Remove a member from the organization.

Examples:
  orochi org remove user@example.com
  orochi org remove user@example.com --force`,
	Args: cobra.ExactArgs(1),
	RunE: runOrgRemove,
}

func init() {
	// Add subcommands
	orgCmd.AddCommand(orgListCmd)
	orgCmd.AddCommand(orgCreateCmd)
	orgCmd.AddCommand(orgGetCmd)
	orgCmd.AddCommand(orgDeleteCmd)
	orgCmd.AddCommand(orgSwitchCmd)
	orgCmd.AddCommand(orgMembersCmd)
	orgCmd.AddCommand(orgInviteCmd)
	orgCmd.AddCommand(orgRemoveCmd)

	// Delete flags
	orgDeleteCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force delete without confirmation")

	// Invite flags
	orgInviteCmd.Flags().StringVar(&orgRole, "role", "member", "Role for the invited user (owner, admin, member, viewer)")

	// Remove flags
	orgRemoveCmd.Flags().BoolVarP(&forceDelete, "force", "f", false, "Force remove without confirmation")
}

func runOrgList(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	client := api.NewClient()
	orgs, err := client.ListOrganizations(cmd.Context())
	if err != nil {
		return fmt.Errorf("failed to list organizations: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"organizations": orgs}, func() {
		output.PrintOrgTable(orgs)
	})

	return nil
}

func runOrgCreate(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]
	// Generate slug from name (lowercase, replace spaces with hyphens, remove special chars)
	slug := strings.ToLower(name)
	slug = strings.ReplaceAll(slug, " ", "-")
	slug = regexp.MustCompile(`[^a-z0-9-]`).ReplaceAllString(slug, "")

	client := api.NewClient()
	org, err := client.CreateOrganization(cmd.Context(), name, slug)
	if err != nil {
		return fmt.Errorf("failed to create organization: %w", err)
	}

	format := getOutputFormat()
	if format == "json" {
		output.PrintJSON(org)
	} else {
		output.Success("Organization '%s' created", name)
		output.Info("Run 'orochi org switch %s' to switch to this organization", org.Slug)
	}

	return nil
}

func runOrgGet(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	var name string
	if len(args) > 0 {
		name = args[0]
	}

	client := api.NewClient()
	org, err := client.GetOrganization(cmd.Context(), name)
	if err != nil {
		return fmt.Errorf("failed to get organization: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, org, func() {
		output.PrintOrgDetails(org)
	})

	return nil
}

func runOrgDelete(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	if !forceDelete {
		output.Warning("You are about to delete organization '%s'", name)
		output.Print("This will delete ALL clusters and data in this organization.")
		if !output.Confirm("Are you sure?") {
			output.Print("Deletion cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.DeleteOrganization(cmd.Context(), name); err != nil {
		return fmt.Errorf("failed to delete organization: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Organization '%s' deleted", name))

	return nil
}

func runOrgSwitch(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	name := args[0]

	// Verify the organization exists
	client := api.NewClient()
	org, err := client.GetOrganization(cmd.Context(), name)
	if err != nil {
		return fmt.Errorf("failed to get organization: %w", err)
	}

	// Store the current organization in config
	if err := config.SetOrganization(org.Slug); err != nil {
		return fmt.Errorf("failed to switch organization: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Switched to organization '%s'", org.Name))

	return nil
}

func runOrgMembers(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	var slug string
	if len(args) > 0 {
		slug = args[0]
	} else {
		// Use current organization from config
		slug = config.GetOrganization()
		if slug == "" {
			return fmt.Errorf("no organization specified. Use 'orochi org switch <name>' to set a default organization")
		}
	}

	client := api.NewClient()
	members, err := client.ListOrgMembers(cmd.Context(), slug)
	if err != nil {
		return fmt.Errorf("failed to list members: %w", err)
	}

	format := getOutputFormat()
	output.FormatOutput(format, map[string]interface{}{"members": members}, func() {
		output.PrintMembersTable(members)
	})

	return nil
}

func runOrgInvite(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	email := args[0]

	// Validate role
	validRoles := map[string]bool{"owner": true, "admin": true, "member": true, "viewer": true}
	if !validRoles[orgRole] {
		return fmt.Errorf("invalid role: %s (must be owner, admin, member, or viewer)", orgRole)
	}

	// Use current organization from config
	slug := config.GetOrganization()
	if slug == "" {
		return fmt.Errorf("no organization selected. Use 'orochi org switch <name>' to set a default organization")
	}

	client := api.NewClient()
	if err := client.InviteOrgMember(cmd.Context(), slug, email, orgRole); err != nil {
		return fmt.Errorf("failed to send invitation: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Invitation sent to '%s' with role '%s'", email, orgRole))

	return nil
}

func runOrgRemove(cmd *cobra.Command, args []string) error {
	if err := requireAuth(); err != nil {
		return err
	}

	email := args[0]

	// Use current organization from config
	slug := config.GetOrganization()
	if slug == "" {
		return fmt.Errorf("no organization selected. Use 'orochi org switch <name>' to set a default organization")
	}

	if !forceDelete {
		output.Warning("You are about to remove '%s' from the organization", email)
		if !output.Confirm("Are you sure?") {
			output.Print("Removal cancelled.")
			return nil
		}
	}

	client := api.NewClient()
	if err := client.RemoveOrgMember(cmd.Context(), slug, email); err != nil {
		return fmt.Errorf("failed to remove member: %w", err)
	}

	format := getOutputFormat()
	output.PrintSuccess(format, fmt.Sprintf("Removed '%s' from organization", email))

	return nil
}
