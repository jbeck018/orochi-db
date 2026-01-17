import * as React from "react";
import { createFileRoute, Link, useNavigate } from "@tanstack/react-router";
import { Database, Loader2, Building2, UserPlus, AlertCircle, CheckCircle } from "lucide-react";
import { useInviteByToken, useAcceptInvite } from "@/hooks/api";
import { isAuthenticated } from "@/lib/auth";
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Alert, AlertDescription } from "@/components/ui/alert";

export const Route = createFileRoute("/invites/$token")({
  component: InviteAcceptPage,
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Accept your organization invitation",
      },
    ],
    title: "Accept Invitation - Orochi Cloud",
  }),
});

function InviteAcceptPage(): React.JSX.Element {
  const { token } = Route.useParams();
  const navigate = useNavigate();
  const { data: invite, isLoading, error } = useInviteByToken(token);
  const acceptMutation = useAcceptInvite();
  const [accepted, setAccepted] = React.useState(false);

  const authenticated = isAuthenticated();

  const handleAccept = async () => {
    try {
      await acceptMutation.mutateAsync(token);
      setAccepted(true);
      // Redirect to clusters after a short delay
      setTimeout(() => {
        navigate({ to: "/clusters" });
      }, 2000);
    } catch (err) {
      // Error will be shown from mutation
    }
  };

  // Loading state
  if (isLoading) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="flex items-center gap-2">
          <Loader2 className="h-6 w-6 animate-spin" />
          <span>Loading invitation...</span>
        </div>
      </div>
    );
  }

  // Invalid/expired invite
  if (error || !invite) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="w-full max-w-md space-y-6">
          <div className="flex flex-col items-center space-y-2 text-center">
            <div className="flex items-center space-x-2">
              <Database className="h-10 w-10 text-primary" />
              <span className="text-3xl font-bold">Orochi Cloud</span>
            </div>
          </div>

          <Card>
            <CardHeader className="space-y-1">
              <div className="flex justify-center mb-4">
                <div className="p-3 rounded-full bg-destructive/10">
                  <AlertCircle className="h-8 w-8 text-destructive" />
                </div>
              </div>
              <CardTitle className="text-2xl text-center">
                Invalid Invitation
              </CardTitle>
              <CardDescription className="text-center">
                This invitation link is invalid or has expired.
              </CardDescription>
            </CardHeader>
            <CardFooter className="flex justify-center">
              <Button asChild>
                <Link to="/">Go to Home</Link>
              </Button>
            </CardFooter>
          </Card>
        </div>
      </div>
    );
  }

  // Already accepted
  if (invite.acceptedAt) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="w-full max-w-md space-y-6">
          <div className="flex flex-col items-center space-y-2 text-center">
            <div className="flex items-center space-x-2">
              <Database className="h-10 w-10 text-primary" />
              <span className="text-3xl font-bold">Orochi Cloud</span>
            </div>
          </div>

          <Card>
            <CardHeader className="space-y-1">
              <div className="flex justify-center mb-4">
                <div className="p-3 rounded-full bg-muted">
                  <CheckCircle className="h-8 w-8 text-muted-foreground" />
                </div>
              </div>
              <CardTitle className="text-2xl text-center">
                Already Accepted
              </CardTitle>
              <CardDescription className="text-center">
                This invitation has already been accepted.
              </CardDescription>
            </CardHeader>
            <CardFooter className="flex justify-center">
              <Button asChild>
                <Link to="/clusters">Go to Dashboard</Link>
              </Button>
            </CardFooter>
          </Card>
        </div>
      </div>
    );
  }

  // Success state
  if (accepted) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="w-full max-w-md space-y-6">
          <div className="flex flex-col items-center space-y-2 text-center">
            <div className="flex items-center space-x-2">
              <Database className="h-10 w-10 text-primary" />
              <span className="text-3xl font-bold">Orochi Cloud</span>
            </div>
          </div>

          <Card>
            <CardHeader className="space-y-1">
              <div className="flex justify-center mb-4">
                <div className="p-3 rounded-full bg-green-100 dark:bg-green-900/20">
                  <CheckCircle className="h-8 w-8 text-green-600" />
                </div>
              </div>
              <CardTitle className="text-2xl text-center">
                Welcome to {invite.organizationName}!
              </CardTitle>
              <CardDescription className="text-center">
                You've successfully joined the organization. Redirecting to dashboard...
              </CardDescription>
            </CardHeader>
          </Card>
        </div>
      </div>
    );
  }

  // Not authenticated - redirect to register
  if (!authenticated) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="w-full max-w-md space-y-6">
          <div className="flex flex-col items-center space-y-2 text-center">
            <div className="flex items-center space-x-2">
              <Database className="h-10 w-10 text-primary" />
              <span className="text-3xl font-bold">Orochi Cloud</span>
            </div>
          </div>

          <Card>
            <CardHeader className="space-y-1">
              <div className="flex justify-center mb-4">
                <div className="p-3 rounded-full bg-primary/10">
                  <Building2 className="h-8 w-8 text-primary" />
                </div>
              </div>
              <CardTitle className="text-2xl text-center">
                You're Invited!
              </CardTitle>
              <CardDescription className="text-center">
                {invite.invitedByName} has invited you to join <strong>{invite.organizationName}</strong> as a{" "}
                <strong>{invite.role}</strong>.
              </CardDescription>
            </CardHeader>
            <CardContent className="space-y-4">
              <div className="rounded-lg border bg-muted/50 p-4 space-y-2">
                <div className="flex items-center gap-2 text-sm">
                  <span className="text-muted-foreground">Email:</span>
                  <span className="font-medium">{invite.email}</span>
                </div>
                <div className="flex items-center gap-2 text-sm">
                  <span className="text-muted-foreground">Role:</span>
                  <span className="font-medium capitalize">{invite.role}</span>
                </div>
              </div>

              <p className="text-sm text-muted-foreground text-center">
                Create an account or sign in to accept this invitation.
              </p>
            </CardContent>
            <CardFooter className="flex flex-col gap-3">
              <Button asChild className="w-full">
                <Link to="/register" search={{ invite: token }}>
                  <UserPlus className="mr-2 h-4 w-4" />
                  Create Account
                </Link>
              </Button>
              <Button variant="outline" asChild className="w-full">
                <Link to="/login">
                  Sign In
                </Link>
              </Button>
            </CardFooter>
          </Card>
        </div>
      </div>
    );
  }

  // Authenticated - show accept button
  return (
    <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
      <div className="w-full max-w-md space-y-6">
        <div className="flex flex-col items-center space-y-2 text-center">
          <div className="flex items-center space-x-2">
            <Database className="h-10 w-10 text-primary" />
            <span className="text-3xl font-bold">Orochi Cloud</span>
          </div>
        </div>

        <Card>
          <CardHeader className="space-y-1">
            <div className="flex justify-center mb-4">
              <div className="p-3 rounded-full bg-primary/10">
                <Building2 className="h-8 w-8 text-primary" />
              </div>
            </div>
            <CardTitle className="text-2xl text-center">
              Join {invite.organizationName}
            </CardTitle>
            <CardDescription className="text-center">
              {invite.invitedByName} has invited you to join this organization as a{" "}
              <strong>{invite.role}</strong>.
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-4">
            {acceptMutation.isError && (
              <Alert variant="destructive">
                <AlertDescription>
                  {acceptMutation.error instanceof Error
                    ? acceptMutation.error.message
                    : "Failed to accept invitation"}
                </AlertDescription>
              </Alert>
            )}

            <div className="rounded-lg border bg-muted/50 p-4 space-y-2">
              <div className="flex items-center gap-2 text-sm">
                <span className="text-muted-foreground">Organization:</span>
                <span className="font-medium">{invite.organizationName}</span>
              </div>
              <div className="flex items-center gap-2 text-sm">
                <span className="text-muted-foreground">Role:</span>
                <span className="font-medium capitalize">{invite.role}</span>
              </div>
              <div className="flex items-center gap-2 text-sm">
                <span className="text-muted-foreground">Invited by:</span>
                <span className="font-medium">{invite.invitedByName}</span>
              </div>
            </div>
          </CardContent>
          <CardFooter className="flex flex-col gap-3">
            <Button
              className="w-full"
              onClick={handleAccept}
              disabled={acceptMutation.isPending}
            >
              {acceptMutation.isPending ? (
                <>
                  <Loader2 className="mr-2 h-4 w-4 animate-spin" />
                  Accepting...
                </>
              ) : (
                <>
                  <CheckCircle className="mr-2 h-4 w-4" />
                  Accept Invitation
                </>
              )}
            </Button>
            <Button variant="ghost" asChild className="w-full">
              <Link to="/clusters">
                Cancel
              </Link>
            </Button>
          </CardFooter>
        </Card>
      </div>
    </div>
  );
}
