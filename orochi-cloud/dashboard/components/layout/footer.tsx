import * as React from "react";
import Link from "next/link";

export function Footer(): React.JSX.Element {
  const currentYear = new Date().getFullYear();

  return (
    <footer className="border-t bg-background">
      <div className="container flex flex-col items-center justify-between gap-4 py-6 md:h-16 md:flex-row md:py-0">
        <div className="text-center text-sm text-muted-foreground md:text-left">
          <p>
            {currentYear} Orochi Cloud. All rights reserved.
          </p>
        </div>
        <div className="flex items-center gap-4 text-sm text-muted-foreground">
          <Link
            href="/docs"
            className="hover:text-foreground transition-colors"
          >
            Documentation
          </Link>
          <Link
            href="/support"
            className="hover:text-foreground transition-colors"
          >
            Support
          </Link>
          <Link
            href="/status"
            className="hover:text-foreground transition-colors"
          >
            Status
          </Link>
          <Link
            href="/privacy"
            className="hover:text-foreground transition-colors"
          >
            Privacy
          </Link>
          <Link
            href="/terms"
            className="hover:text-foreground transition-colors"
          >
            Terms
          </Link>
        </div>
      </div>
    </footer>
  );
}
