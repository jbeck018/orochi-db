"use client";

import * as React from "react";
import { createFileRoute, Link, useNavigate } from "@tanstack/react-router";
import {
  Building2,
  Settings,
  Users,
  UserPlus,
  Loader2,
  AlertCircle,
  Save,
  Trash2,
} from "lucide-react";
import { DashboardLayout } from "@/components/layout/dashboard-layout";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Skeleton } from "@/components/ui/skeleton";
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from "@/components/ui/alert-dialog";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { useToast } from "@/hooks/use-toast";
import {
  useOrganization,
  useOrganizationMembers,
  useUpdateOrganization,
  useDeleteOrganization,
} from "@/hooks/api";
import { MembersList } from "@/components/organizations/members-list";
import { InviteDialog } from "@/components/organizations/invite-dialog";
import { getStoredUser } from "@/lib/auth";
import type { OrganizationRole } from "@/types";

export const Route = createFileRoute("/organizations/$id")({
  component: OrganizationSettingsPage,
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Manage your organization settings and members",
      },
    ],
    title: "Organization Settings - Orochi Cloud",
  }),
});

function OrganizationSettingsPage(): React.JSX.Element {
  const { id: organizationId } = Route.useParams();
  const navigate = useNavigate();
  const { toast } = useToast();

  const { data: organization, isLoading: orgLoading, error: orgError } = useOrganization(organizationId);
  const { data: members = [] } = useOrganizationMembers(organizationId);
  const updateOrgMutation = useUpdateOrganization();
  const deleteOrgMutation = useDeleteOrganization();

  const [name, setName] = React.useState("");
  const [showInviteDialog, setShowInviteDialog] = React.useState(false);
  const [showDeleteDialog, setShowDeleteDialog] = React.useState(false);
  const [deleteConfirmName, setDeleteConfirmName] = React.useState("");

  const currentUser = getStoredUser();

  // Find current user's role in the organization
  const currentMember = members.find((m) => m.userId === currentUser?.id);
  const userRole: OrganizationRole = currentMember?.role ?? "viewer";
  const isOwner = userRole === "owner";
  const isAdmin = userRole === "owner" || userRole === "admin";

  // Initialize form with organization data
  React.useEffect(() => {
    if (organization) {
      setName(organization.name);
    }
  }, [organization]);

  const handleUpdateOrganization = async () => {
    if (!organization) return;

    try {
      await updateOrgMutation.mutateAsync({
        id: organizationId,
        data: { name },
      });
      toast({ title: "Organization updated" });
    } catch (err) {
      toast({
        title: "Error",
        description: err instanceof Error ? err.message : "Failed to update organization",
        variant: "destructive",
      });
    }
  };

  const handleDeleteOrganization = async () => {
    if (!organization || deleteConfirmName !== organization.name) return;

    try {
      await deleteOrgMutation.mutateAsync(organizationId);
      toast({ title: "Organization deleted" });
      navigate({ to: "/clusters" });
    } catch (err) {
      toast({
        title: "Error",
        description: err instanceof Error ? err.message : "Failed to delete organization",
        variant: "destructive",
      });
    }
  };

  if (orgLoading) {
    return (
      <DashboardLayout>
        <div className="max-w-4xl mx-auto space-y-6">
          <Skeleton className="h-10 w-48" />
          <Skeleton className="h-[400px]" />
        </div>
      </DashboardLayout>
    );
  }

  if (orgError || !organization) {
    return (
      <DashboardLayout>
        <div className="flex flex-col items-center justify-center py-20">
          <AlertCircle className="h-12 w-12 text-muted-foreground mb-4" />
          <h2 className="text-xl font-semibold">Organization not found</h2>
          <p className="text-muted-foreground mb-4">
            The organization you're looking for doesn't exist or you don't have access.
          </p>
          <Button asChild>
            <Link to="/clusters">Back to Dashboard</Link>
          </Button>
        </div>
      </DashboardLayout>
    );
  }

  return (
    <DashboardLayout>
      <div className="max-w-4xl mx-auto space-y-6">
        {/* Header */}
        <div className="flex items-start justify-between">
          <div className="flex items-center gap-4">
            <div className="flex h-12 w-12 items-center justify-center rounded-lg bg-primary/10">
              <Building2 className="h-6 w-6 text-primary" />
            </div>
            <div>
              <h1 className="text-3xl font-bold tracking-tight">{organization.name}</h1>
              <p className="text-muted-foreground">
                Manage organization settings and team members
              </p>
            </div>
          </div>
          {isAdmin && (
            <Button onClick={() => setShowInviteDialog(true)}>
              <UserPlus className="mr-2 h-4 w-4" />
              Invite Member
            </Button>
          )}
        </div>

        <Tabs defaultValue="members" className="space-y-6">
          <TabsList>
            <TabsTrigger value="members">
              <Users className="mr-2 h-4 w-4" />
              Members
            </TabsTrigger>
            <TabsTrigger value="settings">
              <Settings className="mr-2 h-4 w-4" />
              Settings
            </TabsTrigger>
          </TabsList>

          {/* Members Tab */}
          <TabsContent value="members" className="space-y-6">
            <MembersList
              organizationId={organizationId}
              currentUserId={currentUser?.id ?? ""}
              userRole={userRole}
            />
          </TabsContent>

          {/* Settings Tab */}
          <TabsContent value="settings" className="space-y-6">
            <Card>
              <CardHeader>
                <CardTitle>General Settings</CardTitle>
                <CardDescription>
                  Update your organization's basic information
                </CardDescription>
              </CardHeader>
              <CardContent className="space-y-4">
                <div className="space-y-2">
                  <Label htmlFor="name">Organization Name</Label>
                  <Input
                    id="name"
                    value={name}
                    onChange={(e) => setName(e.target.value)}
                    disabled={!isAdmin || updateOrgMutation.isPending}
                  />
                </div>
                <div className="space-y-2">
                  <Label>Organization ID</Label>
                  <Input value={organization.id} readOnly className="font-mono text-sm bg-muted" />
                  <p className="text-xs text-muted-foreground">
                    This is your unique organization identifier
                  </p>
                </div>
                {organization.slug && (
                  <div className="space-y-2">
                    <Label>Slug</Label>
                    <Input value={organization.slug} readOnly className="font-mono text-sm bg-muted" />
                  </div>
                )}
                {isAdmin && (
                  <div className="flex justify-end">
                    <Button
                      onClick={handleUpdateOrganization}
                      disabled={updateOrgMutation.isPending || name === organization.name}
                    >
                      {updateOrgMutation.isPending ? (
                        <>
                          <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                          Saving...
                        </>
                      ) : (
                        <>
                          <Save className="mr-2 h-4 w-4" />
                          Save Changes
                        </>
                      )}
                    </Button>
                  </div>
                )}
              </CardContent>
            </Card>

            {isOwner && (
              <Card className="border-destructive">
                <CardHeader>
                  <CardTitle className="text-destructive">Danger Zone</CardTitle>
                  <CardDescription>
                    Irreversible and destructive actions
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <div className="flex items-center justify-between">
                    <div>
                      <p className="font-medium">Delete Organization</p>
                      <p className="text-sm text-muted-foreground">
                        Permanently delete this organization and all its clusters
                      </p>
                    </div>
                    <Button
                      variant="destructive"
                      onClick={() => setShowDeleteDialog(true)}
                    >
                      <Trash2 className="mr-2 h-4 w-4" />
                      Delete Organization
                    </Button>
                  </div>
                </CardContent>
              </Card>
            )}
          </TabsContent>
        </Tabs>

        {/* Invite Dialog */}
        <InviteDialog
          organizationId={organizationId}
          organizationName={organization.name}
          open={showInviteDialog}
          onOpenChange={setShowInviteDialog}
        />

        {/* Delete Organization Dialog */}
        <AlertDialog open={showDeleteDialog} onOpenChange={setShowDeleteDialog}>
          <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>Delete Organization?</AlertDialogTitle>
              <AlertDialogDescription>
                This action cannot be undone. This will permanently delete the
                organization <strong>{organization.name}</strong>, all its
                clusters, and remove all members.
              </AlertDialogDescription>
            </AlertDialogHeader>
            <div className="py-4">
              <Alert variant="destructive">
                <AlertCircle className="h-4 w-4" />
                <AlertDescription>
                  All clusters belonging to this organization will be terminated
                  and all data will be lost.
                </AlertDescription>
              </Alert>
              <div className="mt-4 space-y-2">
                <Label htmlFor="confirmName">
                  Type <strong>{organization.name}</strong> to confirm
                </Label>
                <Input
                  id="confirmName"
                  value={deleteConfirmName}
                  onChange={(e) => setDeleteConfirmName(e.target.value)}
                  placeholder={organization.name}
                />
              </div>
            </div>
            <AlertDialogFooter>
              <AlertDialogCancel onClick={() => setDeleteConfirmName("")}>
                Cancel
              </AlertDialogCancel>
              <AlertDialogAction
                onClick={handleDeleteOrganization}
                className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
                disabled={
                  deleteConfirmName !== organization.name ||
                  deleteOrgMutation.isPending
                }
              >
                {deleteOrgMutation.isPending ? (
                  <Loader2 className="h-4 w-4 animate-spin" />
                ) : (
                  "Delete Organization"
                )}
              </AlertDialogAction>
            </AlertDialogFooter>
          </AlertDialogContent>
        </AlertDialog>
      </div>
    </DashboardLayout>
  );
}
