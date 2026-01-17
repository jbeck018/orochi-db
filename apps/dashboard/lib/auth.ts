import type { AuthTokens, User, LoginCredentials, RegisterCredentials } from "@/types";

const TOKEN_KEY = "orochi_auth_tokens";
const USER_KEY = "orochi_user";

export function getStoredTokens(): AuthTokens | null {
  if (typeof window === "undefined") return null;

  const stored = localStorage.getItem(TOKEN_KEY);
  if (!stored) return null;

  try {
    const tokens = JSON.parse(stored) as AuthTokens;
    // Check if token is expired
    if (tokens.expiresAt < Date.now()) {
      clearAuth();
      return null;
    }
    return tokens;
  } catch {
    return null;
  }
}

export function getStoredUser(): User | null {
  if (typeof window === "undefined") return null;

  const stored = localStorage.getItem(USER_KEY);
  if (!stored) return null;

  try {
    return JSON.parse(stored) as User;
  } catch {
    return null;
  }
}

export function storeAuth(tokens: AuthTokens, user: User): void {
  if (typeof window === "undefined") return;

  localStorage.setItem(TOKEN_KEY, JSON.stringify(tokens));
  localStorage.setItem(USER_KEY, JSON.stringify(user));
}

export function clearAuth(): void {
  if (typeof window === "undefined") return;

  localStorage.removeItem(TOKEN_KEY);
  localStorage.removeItem(USER_KEY);
}

export function isAuthenticated(): boolean {
  const tokens = getStoredTokens();
  return tokens !== null && tokens.expiresAt > Date.now();
}

export function getAuthHeader(): Record<string, string> {
  const tokens = getStoredTokens();
  if (!tokens) return {};
  return { Authorization: `Bearer ${tokens.accessToken}` };
}

// API Functions
const API_URL = import.meta.env.VITE_API_URL ?? "http://localhost:8080";

export async function login(credentials: LoginCredentials): Promise<{ tokens: AuthTokens; user: User }> {
  const response = await fetch(`${API_URL}/api/v1/auth/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(credentials),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: "Login failed" }));
    throw new Error(error.message ?? error.error ?? "Login failed");
  }

  const data = await response.json();

  // Map API response (snake_case) to frontend format (camelCase)
  const tokens: AuthTokens = {
    accessToken: data.access_token,
    refreshToken: data.refresh_token,
    expiresAt: Date.now() + (data.expires_in * 1000), // Convert seconds to absolute timestamp
  };

  const result = {
    tokens,
    user: data.user as User,
  };

  storeAuth(result.tokens, result.user);
  return result;
}

export async function register(credentials: RegisterCredentials): Promise<{ tokens: AuthTokens; user: User }> {
  const response = await fetch(`${API_URL}/api/v1/auth/register`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(credentials),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: "Registration failed" }));
    throw new Error(error.error ?? "Registration failed");
  }

  const data = await response.json();

  // Map API response (snake_case) to frontend format (camelCase)
  const tokens: AuthTokens = {
    accessToken: data.access_token,
    refreshToken: data.refresh_token,
    expiresAt: Date.now() + (data.expires_in * 1000),
  };

  const result = {
    tokens,
    user: data.user as User,
  };

  storeAuth(result.tokens, result.user);
  return result;
}

export async function logout(): Promise<void> {
  const tokens = getStoredTokens();

  if (tokens) {
    try {
      await fetch(`${API_URL}/api/v1/auth/logout`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          ...getAuthHeader(),
        },
      });
    } catch {
      // Ignore logout API errors
    }
  }

  clearAuth();
}

export async function refreshTokens(): Promise<AuthTokens | null> {
  const tokens = getStoredTokens();
  if (!tokens?.refreshToken) return null;

  try {
    const response = await fetch(`${API_URL}/api/v1/auth/refresh`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ refresh_token: tokens.refreshToken }),
    });

    if (!response.ok) {
      clearAuth();
      return null;
    }

    const data = await response.json();

    // Map API response (snake_case) to frontend format (camelCase)
    const newTokens: AuthTokens = {
      accessToken: data.access_token,
      refreshToken: data.refresh_token,
      expiresAt: Date.now() + (data.expires_in * 1000),
    };

    const user = getStoredUser();

    if (user) {
      storeAuth(newTokens, user);
    }

    return newTokens;
  } catch {
    clearAuth();
    return null;
  }
}

export async function getMe(): Promise<User> {
  const response = await fetch(`${API_URL}/api/v1/auth/me`, {
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    throw new Error("Failed to get user");
  }

  const data = await response.json();
  const user = data.user as User;

  const tokens = getStoredTokens();
  if (tokens) {
    storeAuth(tokens, user);
  }

  return user;
}

// OAuth Functions
export type OAuthProvider = "google" | "github";

export function getOAuthUrl(provider: OAuthProvider): string {
  const redirectUri = typeof window !== "undefined"
    ? `${window.location.origin}/auth/callback/${provider}`
    : "";

  return `${API_URL}/api/v1/auth/oauth/${provider}?redirect_uri=${encodeURIComponent(redirectUri)}`;
}

export async function handleOAuthCallback(
  provider: OAuthProvider,
  code: string
): Promise<{ tokens: AuthTokens; user: User }> {
  const redirectUri = typeof window !== "undefined"
    ? `${window.location.origin}/auth/callback/${provider}`
    : "";

  const response = await fetch(`${API_URL}/api/v1/auth/oauth/${provider}/callback`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ code, redirectUri }),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: "OAuth authentication failed" }));
    throw new Error(error.error ?? "OAuth authentication failed");
  }

  const data = await response.json();
  const result = {
    tokens: data.tokens as AuthTokens,
    user: data.user as User,
  };

  storeAuth(result.tokens, result.user);
  return result;
}

export async function linkOAuthProvider(provider: OAuthProvider, code: string): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/auth/oauth/${provider}/link`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
    body: JSON.stringify({ code }),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: "Failed to link account" }));
    throw new Error(error.error ?? "Failed to link account");
  }
}

export async function unlinkOAuthProvider(provider: OAuthProvider): Promise<void> {
  const response = await fetch(`${API_URL}/api/v1/auth/oauth/${provider}/unlink`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      ...getAuthHeader(),
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: "Failed to unlink account" }));
    throw new Error(error.error ?? "Failed to unlink account");
  }
}
