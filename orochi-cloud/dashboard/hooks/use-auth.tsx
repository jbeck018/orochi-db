import * as React from "react";
import { useNavigate } from "@tanstack/react-router";
import {
  getStoredUser,
  getStoredTokens,
  isAuthenticated,
  login as loginFn,
  logout as logoutFn,
  register as registerFn,
} from "@/lib/auth";
import type { User, LoginCredentials, RegisterCredentials, AuthTokens } from "@/types";

interface AuthContextValue {
  user: User | null;
  tokens: AuthTokens | null;
  isAuthenticated: boolean;
  isLoading: boolean;
  login: (credentials: LoginCredentials) => Promise<void>;
  register: (credentials: RegisterCredentials) => Promise<void>;
  logout: () => Promise<void>;
  refresh: () => void;
}

const AuthContext = React.createContext<AuthContextValue | undefined>(undefined);

interface AuthProviderProps {
  children: React.ReactNode;
}

export function AuthProvider({ children }: AuthProviderProps): React.JSX.Element {
  const navigate = useNavigate();
  const [user, setUser] = React.useState<User | null>(null);
  const [tokens, setTokens] = React.useState<AuthTokens | null>(null);
  const [isLoading, setIsLoading] = React.useState(true);

  const refresh = React.useCallback(() => {
    setUser(getStoredUser());
    setTokens(getStoredTokens());
  }, []);

  React.useEffect(() => {
    refresh();
    setIsLoading(false);
  }, [refresh]);

  const login = async (credentials: LoginCredentials): Promise<void> => {
    const result = await loginFn(credentials);
    setUser(result.user);
    setTokens(result.tokens);
  };

  const register = async (credentials: RegisterCredentials): Promise<void> => {
    const result = await registerFn(credentials);
    setUser(result.user);
    setTokens(result.tokens);
  };

  const logout = async (): Promise<void> => {
    await logoutFn();
    setUser(null);
    setTokens(null);
    navigate({ to: "/login" });
  };

  const value: AuthContextValue = {
    user,
    tokens,
    isAuthenticated: isAuthenticated(),
    isLoading,
    login,
    register,
    logout,
    refresh,
  };

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth(): AuthContextValue {
  const context = React.useContext(AuthContext);
  if (context === undefined) {
    throw new Error("useAuth must be used within an AuthProvider");
  }
  return context;
}
