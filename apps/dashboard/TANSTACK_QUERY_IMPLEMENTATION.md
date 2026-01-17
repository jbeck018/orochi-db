# TanStack Query Implementation

## Overview
Successfully implemented TanStack Query (React Query) throughout the Orochi DB dashboard for efficient server state management.

## Installed Packages
- `@tanstack/react-query`: ^5.x
- `@tanstack/react-query-devtools`: ^5.x

## Core Implementation

### 1. Query Client Setup (`lib/queryClient.ts`)
- Configured with optimized defaults:
  - `staleTime`: 5 minutes
  - `gcTime` (garbage collection): 10 minutes
  - Smart retry logic (no retry on 4xx errors)
  - Automatic refetch on window focus and reconnect

### 2. Provider Integration (`src/routes/__root.tsx`)
- Wrapped application with `QueryClientProvider`
- Added `ReactQueryDevtools` in development mode
- Integrated seamlessly with existing ThemeProvider

### 3. API Hooks (`hooks/api/`)

#### Cluster Hooks (`useClusters.ts`)
- **Queries:**
  - `useClusters(page, pageSize)` - List all clusters with pagination
  - `useCluster(id)` - Get single cluster details
  - `useClusterMetrics(id)` - Real-time metrics (10s refetch interval)
  - `useClusterMetricsHistory(id, period)` - Historical metrics
  - `useConnectionString(id)` - Connection string (cached infinitely)

- **Mutations with Optimistic Updates:**
  - `useCreateCluster()` - Create new cluster
  - `useUpdateCluster(id)` - Update cluster (optimistic)
  - `useDeleteCluster()` - Delete cluster (optimistic removal from list)
  - `useScaleCluster(id)` - Scale nodes (optimistic update)
  - `useStartCluster(id)` - Start cluster (optimistic status change)
  - `useStopCluster(id)` - Stop cluster (optimistic status change)
  - `useRestartCluster(id)` - Restart cluster
  - `useResetClusterPassword(id)` - Reset cluster password

#### User Hooks (`useUser.ts`)
- **Queries:**
  - `useUser()` - Current user data
  - `useProviders()` - Cloud provider options (1h cache)
  - `useTiers()` - Tier options (1h cache)
  - `useSystemHealth()` - System health (1m refetch)
  - `useNotificationPreferences()` - Notification settings
  - `useTwoFactorStatus()` - 2FA status

- **Mutations:**
  - `useUpdateUser()` - Update user profile (optimistic)
  - `useChangePassword()` - Change password
  - `useDeleteAccount()` - Delete account (clears all cache)
  - `useUpdateNotificationPreferences()` - Update notifications (optimistic)
  - `useEnableTwoFactor()` - Enable 2FA
  - `useVerifyTwoFactor()` - Verify 2FA setup
  - `useDisableTwoFactor()` - Disable 2FA

#### Auth Hooks (`useAuth.ts`)
- `useLogin()` - Login with automatic navigation
- `useRegister()` - Register with automatic navigation
- `useLogout()` - Logout with cache clearing
- `useForgotPassword()` - Request password reset
- `useResetPassword()` - Reset password with token

## Updated Components

### Pages Updated:
1. **`src/routes/clusters.index.tsx`**
   - Replaced manual `useState` + `useEffect` + `fetch` with `useClusters()`
   - Removed manual refresh logic (now uses `refetch()`)
   - Automatic 30s polling via query configuration
   - Simplified loading and error states

2. **`src/routes/clusters.$id.tsx`**
   - Replaced manual data fetching with `useCluster()`
   - Added `useClusterMetrics()` for real-time metrics
   - Added `useClusterMetricsHistory()` for charts
   - Updated actions to use mutations
   - Removed manual `useEffect` and interval management

3. **`components/clusters/cluster-card.tsx`**
   - Integrated `useClusterMetrics()` for per-cluster metrics
   - Replaced manual API calls with mutations:
     - `useStartCluster()`
     - `useStopCluster()`
     - `useRestartCluster()`
     - `useDeleteCluster()`
   - Automatic loading states from `isPending`

4. **`components/auth/login-form.tsx`**
   - Replaced manual form submission with `useLogin()`
   - Automatic error handling via `loginMutation.error`
   - Loading state via `loginMutation.isPending`

5. **`components/auth/register-form.tsx`**
   - Replaced manual registration with `useRegister()`
   - Automatic navigation on success
   - Combined validation and mutation errors

## Key Benefits Achieved

### 1. Automatic Caching
- Eliminates duplicate requests
- Reduces server load
- Instant UI updates for cached data

### 2. Optimistic Updates
- **Cluster deletion**: Immediate removal from UI
- **Cluster scaling**: Instant node count update
- **Status changes**: Immediate status reflection
- Automatic rollback on errors

### 3. Background Refetching
- Cluster list: 30s stale time
- Metrics: 10s refetch interval
- Configurable per-query basis

### 4. Better UX
- Loading states: `isLoading`, `isPending`, `isRefetching`
- Error handling: Automatic error states
- No more race conditions from manual `useEffect`

### 5. Developer Experience
- Declarative data fetching
- No manual cache management
- DevTools for debugging
- Type-safe query keys

## Query Key Structure

All query keys are centralized for easy invalidation:

```typescript
clusterKeys = {
  all: ['clusters'],
  lists: () => ['clusters', 'list'],
  list: (page, pageSize) => ['clusters', 'list', { page, pageSize }],
  detail: (id) => ['clusters', 'detail', id],
  metrics: (id) => ['clusters', 'detail', id, 'metrics'],
  // ...
}
```

## Optimistic Update Examples

### Delete Cluster
```typescript
onMutate: async (id) => {
  await queryClient.cancelQueries({ queryKey: clusterKeys.lists() });
  const previousClusters = queryClient.getQueryData(clusterKeys.lists());
  
  // Optimistically remove
  queryClient.setQueriesData({ queryKey: clusterKeys.lists() }, (old) => ({
    ...old,
    data: old.data.filter(c => c.id !== id)
  }));
  
  return { previousClusters };
},
onError: (err, id, context) => {
  // Rollback on error
  queryClient.setQueriesData(clusterKeys.lists(), context.previousClusters);
}
```

### Scale Cluster
```typescript
onMutate: async (nodeCount) => {
  const previous = queryClient.getQueryData(clusterKeys.detail(id));
  
  // Optimistically update node count
  queryClient.setQueryData(clusterKeys.detail(id), (old) => ({
    ...old,
    data: {
      ...old.data,
      config: { ...old.data.config, nodeCount }
    }
  }));
  
  return { previous };
}
```

## DevTools
In development mode, access React Query DevTools:
- Bottom-left corner toggle
- View all queries and mutations
- Inspect cache state
- Manually invalidate queries
- Monitor network requests

## Migration Notes

### Before (Manual Fetch)
```typescript
const [data, setData] = useState([]);
const [loading, setLoading] = useState(true);

useEffect(() => {
  async function fetchData() {
    setLoading(true);
    const result = await api.get();
    setData(result.data);
    setLoading(false);
  }
  fetchData();
}, []);
```

### After (TanStack Query)
```typescript
const { data, isLoading } = useQuery({
  queryKey: ['data'],
  queryFn: () => api.get()
});
```

## Next Steps (Optional)
1. Add infinite scroll for clusters list using `useInfiniteQuery`
2. Implement prefetching for faster navigation
3. Add persistence to localStorage with `persistQueryClient`
4. Create custom hooks for complex data transformations
5. Add request deduplication for concurrent requests
6. Implement retry strategies for specific errors

## Resources
- [TanStack Query Docs](https://tanstack.com/query/latest)
- [React Query DevTools](https://tanstack.com/query/latest/docs/react/devtools)
- [Optimistic Updates](https://tanstack.com/query/latest/docs/react/guides/optimistic-updates)
