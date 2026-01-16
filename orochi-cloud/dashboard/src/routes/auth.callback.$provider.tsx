import * as React from "react";
import { createFileRoute, Link, useNavigate } from "@tanstack/react-router";
import { Loader2, AlertCircle } from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { handleOAuthCallback, type OAuthProvider } from "@/lib/auth";

export const Route = createFileRoute("/auth/callback/$provider")({
  component: OAuthCallbackPage,
  validateSearch: (
    search: Record<string, unknown>
  ): { code?: string; error?: string; error_description?: string } => {
    return {
      code: typeof search.code === "string" ? search.code : undefined,
      error: typeof search.error === "string" ? search.error : undefined,
      error_description:
        typeof search.error_description === "string"
          ? search.error_description
          : undefined,
    };
  },
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Completing OAuth authentication",
      },
    ],
    title: "Authenticating - Orochi Cloud",
  }),
});

function OAuthCallbackPage(): React.JSX.Element {
  const navigate = useNavigate();
  const { provider } = Route.useParams();
  const { code, error, error_description: errorDescription } = Route.useSearch();

  const [isLoading, setIsLoading] = React.useState(true);
  const [authError, setAuthError] = React.useState<string | null>(null);

  React.useEffect(() => {
    const authenticate = async (): Promise<void> => {
      if (error) {
        setAuthError(errorDescription ?? error ?? "Authentication failed");
        setIsLoading(false);
        return;
      }

      if (!code) {
        setAuthError("No authorization code received");
        setIsLoading(false);
        return;
      }

      if (!["google", "github"].includes(provider)) {
        setAuthError("Invalid OAuth provider");
        setIsLoading(false);
        return;
      }

      try {
        await handleOAuthCallback(provider as OAuthProvider, code);
        navigate({ to: "/" });
      } catch (err) {
        setAuthError(
          err instanceof Error ? err.message : "Authentication failed"
        );
        setIsLoading(false);
      }
    };

    authenticate();
  }, [code, error, errorDescription, provider, navigate]);

  if (isLoading) {
    return (
      <div className="min-h-screen flex items-center justify-center p-4 bg-gradient-to-br from-background to-muted">
        <Card className="w-full max-w-md">
          <CardContent className="flex flex-col items-center justify-center py-12">
            <Loader2 className="h-12 w-12 animate-spin text-primary mb-4" />
            <p className="text-lg font-medium">Completing sign in...</p>
            <p className="text-sm text-muted-foreground">
              Please wait while we verify your credentials.
            </p>
          </CardContent>
        </Card>
      </div>
    );
  }

  return (
    <div className="min-h-screen flex items-center justify-center p-4 bg-gradient-to-br from-background to-muted">
      <Card className="w-full max-w-md">
        <CardHeader className="text-center">
          <div className="mx-auto mb-4 flex h-12 w-12 items-center justify-center rounded-full bg-destructive/10">
            <AlertCircle className="h-6 w-6 text-destructive" />
          </div>
          <CardTitle>Authentication Failed</CardTitle>
          <CardDescription>
            {authError ?? "An error occurred during authentication."}
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          <Link to="/login">
            <Button className="w-full">Try again</Button>
          </Link>
          <p className="text-center text-sm text-muted-foreground">
            Having trouble?{" "}
            <a
              href="mailto:support@orochi.dev"
              className="text-primary hover:underline"
            >
              Contact support
            </a>
          </p>
        </CardContent>
      </Card>
    </div>
  );
}
