"use client";

import * as React from "react";

type Theme = "dark" | "light" | "system";

interface ThemeProviderProps {
  children: React.ReactNode;
  defaultTheme?: Theme;
  storageKey?: string;
  attribute?: string;
  enableSystem?: boolean;
  disableTransitionOnChange?: boolean;
}

interface ThemeContextValue {
  theme: Theme;
  setTheme: (theme: Theme) => void;
  resolvedTheme: "dark" | "light";
}

const ThemeContext = React.createContext<ThemeContextValue | undefined>(undefined);

function getSystemTheme(): "dark" | "light" {
  if (typeof window === "undefined") return "light";
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

export function ThemeProvider({
  children,
  defaultTheme = "system",
  storageKey = "orochi-theme",
  attribute = "class",
  enableSystem = true,
  disableTransitionOnChange = false,
}: ThemeProviderProps): React.JSX.Element {
  const [theme, setThemeState] = React.useState<Theme>(() => {
    if (typeof window === "undefined") return defaultTheme;
    try {
      const stored = localStorage.getItem(storageKey) as Theme | null;
      return stored ?? defaultTheme;
    } catch {
      return defaultTheme;
    }
  });

  const [resolvedTheme, setResolvedTheme] = React.useState<"dark" | "light">(() => {
    if (typeof window === "undefined") return "light";
    if (theme === "system") return getSystemTheme();
    return theme;
  });

  // Apply theme to document
  const applyTheme = React.useCallback((newTheme: "dark" | "light") => {
    if (typeof window === "undefined") return;

    const root = window.document.documentElement;

    if (disableTransitionOnChange) {
      root.style.setProperty("transition", "none");
    }

    if (attribute === "class") {
      root.classList.remove("light", "dark");
      root.classList.add(newTheme);
    } else {
      root.setAttribute(attribute, newTheme);
    }

    if (disableTransitionOnChange) {
      // Force reflow
      void root.offsetHeight;
      root.style.removeProperty("transition");
    }
  }, [attribute, disableTransitionOnChange]);

  // Set theme and persist
  const setTheme = React.useCallback((newTheme: Theme) => {
    setThemeState(newTheme);
    try {
      localStorage.setItem(storageKey, newTheme);
    } catch {
      // localStorage not available
    }
  }, [storageKey]);

  // Handle system theme changes
  React.useEffect(() => {
    if (!enableSystem) return;

    const mediaQuery = window.matchMedia("(prefers-color-scheme: dark)");

    const handleChange = (): void => {
      if (theme === "system") {
        const systemTheme = getSystemTheme();
        setResolvedTheme(systemTheme);
        applyTheme(systemTheme);
      }
    };

    mediaQuery.addEventListener("change", handleChange);
    return () => mediaQuery.removeEventListener("change", handleChange);
  }, [theme, enableSystem, applyTheme]);

  // Apply theme on mount and when theme changes
  React.useEffect(() => {
    const resolved = theme === "system" ? getSystemTheme() : theme;
    setResolvedTheme(resolved);
    applyTheme(resolved);
  }, [theme, applyTheme]);

  // Prevent flash by applying theme immediately on mount
  React.useEffect(() => {
    // Read from localStorage immediately on mount
    try {
      const stored = localStorage.getItem(storageKey) as Theme | null;
      const initialTheme = stored ?? defaultTheme;
      const resolved = initialTheme === "system" ? getSystemTheme() : initialTheme;
      applyTheme(resolved);
    } catch {
      // localStorage not available
    }
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const value = React.useMemo(
    () => ({ theme, setTheme, resolvedTheme }),
    [theme, setTheme, resolvedTheme]
  );

  return (
    <ThemeContext.Provider value={value}>
      {children}
    </ThemeContext.Provider>
  );
}

export function useTheme(): ThemeContextValue {
  const context = React.useContext(ThemeContext);
  if (context === undefined) {
    throw new Error("useTheme must be used within a ThemeProvider");
  }
  return context;
}
