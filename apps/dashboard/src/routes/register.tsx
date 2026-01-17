import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Database, Loader2 } from "lucide-react";
import { RegisterForm } from "@/components/auth/register-form";
import { useInviteByToken } from "@/hooks/api";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Alert, AlertDescription } from "@/components/ui/alert";

interface RegisterSearchParams {
  invite?: string;
}

export const Route = createFileRoute("/register")({
  component: RegisterPage,
  validateSearch: (search: Record<string, unknown>): RegisterSearchParams => ({
    invite: typeof search.invite === "string" ? search.invite : undefined,
  }),
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Create your Orochi Cloud account",
      },
    ],
    title: "Sign Up - Orochi Cloud",
  }),
});

function RegisterPage(): React.JSX.Element {
  const { invite: inviteToken } = Route.useSearch();
  const { data: invite, isLoading: isLoadingInvite, error: inviteError } = useInviteByToken(inviteToken ?? "");

  // Show loading while fetching invite details
  if (inviteToken && isLoadingInvite) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
        <div className="flex items-center gap-2">
          <Loader2 className="h-6 w-6 animate-spin" />
          <span>Loading invitation...</span>
        </div>
      </div>
    );
  }

  const hasValidInvite = inviteToken && invite && !inviteError;

  return (
    <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-background to-muted p-4">
      <div className="w-full max-w-md space-y-6">
        <div className="flex flex-col items-center space-y-2 text-center">
          <div className="flex items-center space-x-2">
            <Database className="h-10 w-10 text-primary" />
            <span className="text-3xl font-bold">Orochi Cloud</span>
          </div>
          <p className="text-muted-foreground">PostgreSQL HTAP Platform</p>
        </div>

        {inviteToken && inviteError && (
          <Alert variant="destructive">
            <AlertDescription>
              This invitation link is invalid or has expired. You can still create an account with a new organization.
            </AlertDescription>
          </Alert>
        )}

        <Card>
          <CardHeader className="space-y-1">
            <CardTitle className="text-2xl text-center">
              {hasValidInvite ? "Accept Invitation" : "Create an account"}
            </CardTitle>
            <CardDescription className="text-center">
              {hasValidInvite
                ? `Join ${invite.organizationName} on Orochi Cloud`
                : "Get started with Orochi Cloud today"}
            </CardDescription>
          </CardHeader>
          <CardContent>
            <RegisterForm
              inviteToken={hasValidInvite ? inviteToken : undefined}
              inviteEmail={hasValidInvite ? invite.email : undefined}
              organizationName={hasValidInvite ? invite.organizationName : undefined}
            />
          </CardContent>
        </Card>
      </div>
    </div>
  );
}
