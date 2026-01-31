import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Loader2 } from "lucide-react";
import { RegisterForm } from "@/components/auth/register-form";
import howleropsLogo from "@/src/assets/howlerops-icon.png";
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
        content: "Create your HowlerOps account",
      },
    ],
    title: "Sign Up - HowlerOps",
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
          <div className="flex items-center space-x-3">
            <img src={howleropsLogo} alt="HowlerOps" className="h-12 w-12" />
            <span className="text-3xl font-bold">HowlerOps</span>
          </div>
          <p className="text-muted-foreground">OrochiDB - PostgreSQL HTAP Platform</p>
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
                ? `Join ${invite.organizationName} on HowlerOps`
                : "Get started with HowlerOps today"}
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
