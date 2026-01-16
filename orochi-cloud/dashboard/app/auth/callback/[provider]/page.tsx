"use client";

import * as React from "react";
import { useRouter, useSearchParams, useParams } from "next/navigation";
import { Loader2, AlertCircle } from "lucide-react";
import Link from "next/link";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { handleOAuthCallback, type OAuthProvider } from "@/lib/auth";

export default function OAuthCallbackPage(): React.JSX.Element {
  const router = useRouter();
  const searchParams = useSearchParams();
  const params = useParams();

  const provider = params.provider as OAuthProvider;
  const code = searchParams.get("code");
  const error = searchParams.get("error");
  const errorDescription = searchParams.get("error_description");

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
        await handleOAuthCallback(provider, code);
        router.push("/");
      } catch (err) {
        setAuthError(err instanceof Error ? err.message : "Authentication failed");
        setIsLoading(false);
      }
    };

    authenticate();
  }, [code, error, errorDescription, provider, router]);

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
          <Link href="/login">
            <Button className="w-full">Try again</Button>
          </Link>
          <p className="text-center text-sm text-muted-foreground">
            Having trouble?{" "}
            <Link href="mailto:support@orochi.dev" className="text-primary hover:underline">
              Contact support
            </Link>
          </p>
        </CardContent>
      </Card>
    </div>
  );
}
