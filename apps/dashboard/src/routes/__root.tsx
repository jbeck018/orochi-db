import * as React from "react";
import {
  createRootRoute,
  Outlet,
  HeadContent,
  Scripts,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { ReactQueryDevtools } from "@tanstack/react-query-devtools";
import { ThemeProvider } from "@/components/layout/theme-provider";
import { Toaster } from "@/components/ui/toaster";
import { queryClient } from "@/lib/queryClient";
// Import Fira Code font via JS for reliable bundling
import "@fontsource-variable/fira-code";
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
          "HowlerOps - Managed OrochiDB PostgreSQL HTAP platform with automatic sharding, time-series optimization, and columnar storage.",
      },
      {
        name: "keywords",
        content: "PostgreSQL, HTAP, database, cloud, managed, OrochiDB, HowlerOps",
      },
    ],
    links: [
      { rel: "icon", type: "image/png", href: "/favicon.png" },
      { rel: "apple-touch-icon", href: "/apple-touch-icon.png" },
    ],
    title: "HowlerOps - OrochiDB PostgreSQL HTAP Platform",
  }),
});

function RootLayout(): React.JSX.Element {
  return (
    <html lang="en">
      <head>
        <HeadContent />
      </head>
      <body>
        <QueryClientProvider client={queryClient}>
          <ThemeProvider
            attribute="class"
            defaultTheme="system"
            enableSystem
            disableTransitionOnChange
          >
            <Outlet />
            <Toaster />
          </ThemeProvider>
          {import.meta.env.DEV && <ReactQueryDevtools initialIsOpen={false} />}
        </QueryClientProvider>
        <Scripts />
      </body>
    </html>
  );
}
