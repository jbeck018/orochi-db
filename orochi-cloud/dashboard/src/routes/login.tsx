import * as React from "react";
import { createFileRoute } from "@tanstack/react-router";
import { Database } from "lucide-react";
import { LoginForm } from "@/components/auth/login-form";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";

export const Route = createFileRoute("/login")({
  component: LoginPage,
  head: () => ({
    meta: [
      {
        name: "description",
        content: "Sign in to your Orochi Cloud account",
      },
    ],
    title: "Sign In - Orochi Cloud",
  }),
});

function LoginPage(): React.JSX.Element {
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

        <Card>
          <CardHeader className="space-y-1">
            <CardTitle className="text-2xl text-center">Welcome back</CardTitle>
            <CardDescription className="text-center">
              Sign in to your account to continue
            </CardDescription>
          </CardHeader>
          <CardContent>
            <LoginForm />
          </CardContent>
        </Card>
      </div>
    </div>
  );
}
