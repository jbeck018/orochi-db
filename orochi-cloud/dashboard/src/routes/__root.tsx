import * as React from "react";
import {
  createRootRoute,
  Outlet,
  HeadContent,
  Scripts,
} from "@tanstack/react-router";
import { ThemeProvider } from "@/components/layout/theme-provider";
import { Toaster } from "@/components/ui/toaster";
import "../styles/globals.css";

export const Route = createRootRoute({
  component: RootLayout,
  head: () => ({
    meta: [
      { charSet: "utf-8" },
      { name: "viewport", content: "width=device-width, initial-scale=1" },
      {
        name: "description",
        content:
          "Managed PostgreSQL HTAP database platform with automatic sharding, time-series optimization, and columnar storage.",
      },
      {
        name: "keywords",
        content: "PostgreSQL, HTAP, database, cloud, managed",
      },
    ],
    title: "Orochi Cloud - PostgreSQL HTAP Platform",
  }),
});

function RootLayout(): React.JSX.Element {
  return (
    <html lang="en">
      <head>
        <HeadContent />
      </head>
      <body>
        <ThemeProvider
          attribute="class"
          defaultTheme="system"
          enableSystem
          disableTransitionOnChange
        >
          <Outlet />
          <Toaster />
        </ThemeProvider>
        <Scripts />
      </body>
    </html>
  );
}
