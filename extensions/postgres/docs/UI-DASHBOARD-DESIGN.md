# Orochi Cloud UI/Dashboard Design

## Overview

A modern, responsive web dashboard for managing Orochi Cloud database instances. Inspired by PlanetScale, Neon, and Supabase.

---

## 1. Information Architecture

### Navigation Structure

```
┌─────────────────────────────────────────────────────────────┐
│  [Logo] Orochi Cloud    [Org Switcher ▼]  [? Help] [Avatar]│
├─────────────────────────────────────────────────────────────┤
│  Sidebar               │  Main Content Area                 │
│  ────────────          │                                    │
│  Dashboard             │                                    │
│  Databases             │                                    │
│    └─ [db-name]        │                                    │
│  Backups               │                                    │
│  Team                  │                                    │
│  Settings              │                                    │
│    ├─ Organization     │                                    │
│    ├─ Billing          │                                    │
│    └─ API Keys         │                                    │
│  ────────────          │                                    │
│  Docs                  │                                    │
│  Support               │                                    │
└─────────────────────────────────────────────────────────────┘
```

### Page Hierarchy

```
/
├── /login
├── /signup
├── /forgot-password
├── /verify-email
├── /dashboard                     # Organization overview
├── /databases
│   ├── /new                       # Create database wizard
│   └── /[db-id]
│       ├── /                      # Database dashboard
│       ├── /query                 # SQL editor
│       ├── /schema                # Schema browser
│       ├── /metrics               # Performance metrics
│       ├── /logs                  # Query & error logs
│       ├── /backups               # Backup management
│       ├── /branches              # Database branches
│       └── /settings              # Database settings
├── /team
│   ├── /members                   # Team members list
│   ├── /invitations               # Pending invitations
│   └── /roles                     # Role management
├── /settings
│   ├── /organization              # Org settings
│   ├── /billing                   # Billing & usage
│   ├── /api-keys                  # API key management
│   ├── /security                  # SSO, MFA settings
│   └── /audit-log                 # Audit trail
└── /docs                          # Documentation
```

---

## 2. Core Pages/Views

### Authentication Pages

#### Login (`/login`)
```
┌────────────────────────────────────────────┐
│                                            │
│             [Orochi Logo]                  │
│                                            │
│         Welcome back                       │
│         Sign in to your account            │
│                                            │
│  ┌──────────────────────────────────────┐  │
│  │ Email                                │  │
│  └──────────────────────────────────────┘  │
│  ┌──────────────────────────────────────┐  │
│  │ Password                     [Show]  │  │
│  └──────────────────────────────────────┘  │
│                                            │
│  [✓] Remember me     [Forgot password?]    │
│                                            │
│  ┌──────────────────────────────────────┐  │
│  │           Sign In                    │  │
│  └──────────────────────────────────────┘  │
│                                            │
│  ─────────── or continue with ───────────  │
│                                            │
│  [GitHub]  [Google]  [SSO]                 │
│                                            │
│  Don't have an account? Sign up            │
│                                            │
└────────────────────────────────────────────┘
```

#### Signup (`/signup`)
- Email, password, confirm password
- Organization name (auto-generated from email domain)
- Terms of service checkbox
- OAuth options (GitHub, Google)

### Organization Dashboard (`/dashboard`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  Dashboard                                           [+ New Database]│
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐      │
│  │ Databases       │  │ Storage Used    │  │ Compute Hours   │      │
│  │      3          │  │   12.4 GB       │  │   456 hrs       │      │
│  │ ↑ 1 this week   │  │ of 50 GB        │  │ of 1000 hrs     │      │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘      │
│                                                                     │
│  Recent Databases                              [View All →]         │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ ● production-db    us-east-1    Running    2.1 GB    3h ago │    │
│  │ ○ staging-db       us-east-1    Sleeping   0.5 GB    2d ago │    │
│  │ ● analytics-db     eu-west-1    Running    9.8 GB    1m ago │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Activity                                                           │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ [Chart: QPS over last 24 hours across all databases]        │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Quick Actions                                                      │
│  [Create Database] [Invite Team Member] [View Docs]                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Database List (`/databases`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  Databases                                         [+ New Database] │
├─────────────────────────────────────────────────────────────────────┤
│  [Search databases...]  [Region ▼]  [Status ▼]  [Grid│List]         │
│                                                                     │
│  ┌─────────────────────┐  ┌─────────────────────┐                   │
│  │ production-db       │  │ staging-db          │                   │
│  │ ───────────────     │  │ ───────────────     │                   │
│  │ ● Running           │  │ ○ Sleeping          │                   │
│  │ us-east-1           │  │ us-east-1           │                   │
│  │                     │  │                     │                   │
│  │ CPU  [████░░] 65%   │  │ CPU  [░░░░░░] 0%    │                   │
│  │ Mem  [███░░░] 48%   │  │ Mem  [░░░░░░] 0%    │                   │
│  │                     │  │                     │                   │
│  │ 2.1 GB storage      │  │ 0.5 GB storage      │                   │
│  │ 12 connections      │  │ 0 connections       │                   │
│  │                     │  │                     │                   │
│  │ [Connect] [•••]     │  │ [Wake Up] [•••]     │                   │
│  └─────────────────────┘  └─────────────────────┘                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Database Dashboard (`/databases/[id]`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ← Databases  /  production-db                            [•••]     │
├─────────────────────────────────────────────────────────────────────┤
│  [Overview] [Query] [Schema] [Metrics] [Logs] [Backups] [Settings]  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Status: ● Running          Region: us-east-1          Plan: Pro    │
│                                                                     │
│  Connection Details                                        [Copy]   │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ Host:     prod-db-abc123.orochi.cloud                       │    │
│  │ Port:     5432                                              │    │
│  │ Database: main                                              │    │
│  │ User:     admin                                             │    │
│  │ Password: ••••••••••••                           [Show]     │    │
│  │                                                             │    │
│  │ Connection String:                                          │    │
│  │ postgresql://admin:****@prod-db-abc123.orochi.cloud:5432/.. │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Metrics (Last 24 Hours)                                            │
│  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐        │
│  │ CPU Usage       │ │ Memory          │ │ Connections     │        │
│  │ [Sparkline]     │ │ [Sparkline]     │ │ [Sparkline]     │        │
│  │ Avg: 42%        │ │ Avg: 2.1 GB     │ │ Active: 12      │        │
│  └─────────────────┘ └─────────────────┘ └─────────────────┘        │
│  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐        │
│  │ QPS             │ │ Storage         │ │ Query Latency   │        │
│  │ [Sparkline]     │ │ [Sparkline]     │ │ [Sparkline]     │        │
│  │ Peak: 1.2K      │ │ 2.1 GB / 50 GB  │ │ P99: 45ms       │        │
│  └─────────────────┘ └─────────────────┘ └─────────────────┘        │
│                                                                     │
│  Recent Activity                                                    │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ 10:32 AM  Schema migration completed                        │    │
│  │ 09:15 AM  Backup completed successfully                     │    │
│  │ Yesterday Auto-scaled from small to medium                  │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### SQL Editor (`/databases/[id]/query`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ← production-db  /  Query Editor                                   │
├─────────────────────────────────────────────────────────────────────┤
│  [Saved Queries ▼]  [History]  [Format]          [Run ▶] (⌘+Enter)  │
├───────────────────────────────────┬─────────────────────────────────┤
│  Schema Browser                   │  -- Query Editor                │
│  ▼ Tables                         │  SELECT                         │
│    ├─ users                       │    u.id,                        │
│    │   ├─ id (int8, PK)          │    u.name,                      │
│    │   ├─ email (text)           │    COUNT(o.id) as order_count   │
│    │   └─ created_at (timestamptz)│  FROM users u                   │
│    ├─ orders                      │  LEFT JOIN orders o             │
│    │   └─ ...                    │    ON u.id = o.user_id          │
│    └─ products                    │  GROUP BY u.id, u.name          │
│  ▶ Views                          │  ORDER BY order_count DESC      │
│  ▶ Functions                      │  LIMIT 100;                     │
│  ▶ Indexes                        │                                 │
├───────────────────────────────────┴─────────────────────────────────┤
│  Results (156 rows, 23ms)                      [Export CSV] [JSON]  │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  id    │  name           │  order_count                      │   │
│  ├────────┼─────────────────┼───────────────────────────────────┤   │
│  │  1     │  John Doe       │  47                               │   │
│  │  2     │  Jane Smith     │  32                               │   │
│  │  3     │  Bob Wilson     │  28                               │   │
│  │  ...   │  ...            │  ...                              │   │
│  └──────────────────────────────────────────────────────────────┘   │
│  [< 1 2 3 ... 16 >]                                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### Schema Browser (`/databases/[id]/schema`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ← production-db  /  Schema                                         │
├─────────────────────────────────────────────────────────────────────┤
│  [Search tables...]                              [+ Create Table]   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Tables (12)                                                        │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ users                                                       │    │
│  │ 156,432 rows • 24 MB • Last modified: 2 hours ago           │    │
│  │ [View Data] [Edit Schema] [•••]                             │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │ Column         Type              Nullable   Default         │    │
│  │ ─────────────────────────────────────────────────────────── │    │
│  │ id             bigint            NO         nextval(...)    │    │
│  │ email          text              NO                         │    │
│  │ name           text              YES                        │    │
│  │ password_hash  text              NO                         │    │
│  │ created_at     timestamptz       NO         now()           │    │
│  │ updated_at     timestamptz       NO         now()           │    │
│  │                                                             │    │
│  │ Indexes                                                     │    │
│  │ • users_pkey (PRIMARY KEY) on id                            │    │
│  │ • users_email_key (UNIQUE) on email                         │    │
│  │                                                             │    │
│  │ Foreign Keys                                                │    │
│  │ • orders.user_id → users.id                                 │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ orders                                                      │    │
│  │ 1,234,567 rows • 156 MB • Last modified: 5 minutes ago      │    │
│  │ [View Data] [Edit Schema] [•••]                             │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Metrics Dashboard (`/databases/[id]/metrics`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ← production-db  /  Metrics                                        │
├─────────────────────────────────────────────────────────────────────┤
│  Time Range: [Last 24 Hours ▼]  [1H] [6H] [24H] [7D] [30D] [Custom] │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  CPU Utilization                                         Avg: 42%   │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                     ╱╲    ╱╲                                │    │
│  │ 80% ─ ─ ─ ─ ─ ─ ─ ╱──╲──╱──╲─ ─ ─ ─ ─ ─ ─ [Scale Up]      │    │
│  │              ╱╲  ╱      ╲╱                                  │    │
│  │ 40% ───────╱──╲╱─────────────────────────────────────────   │    │
│  │     ╱╲   ╱                                                  │    │
│  │ 0% ╱──╲─╱─────────────────────────────────────────────────  │    │
│  │    00:00    04:00    08:00    12:00    16:00    20:00       │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌────────────────────────────┐  ┌────────────────────────────┐     │
│  │ Memory Usage               │  │ Active Connections         │     │
│  │ [Chart]                    │  │ [Chart]                    │     │
│  │ Current: 2.1 GB / 4 GB     │  │ Current: 12 / 100          │     │
│  └────────────────────────────┘  └────────────────────────────┘     │
│                                                                     │
│  ┌────────────────────────────┐  ┌────────────────────────────┐     │
│  │ Queries per Second         │  │ Query Latency (P99)        │     │
│  │ [Chart]                    │  │ [Chart]                    │     │
│  │ Peak: 1,234 QPS            │  │ Current: 45ms              │     │
│  └────────────────────────────┘  └────────────────────────────┘     │
│                                                                     │
│  Top Queries by Time                                                │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ Query                          Calls    Avg Time   Total    │    │
│  │ SELECT * FROM orders WHERE..   12,456   45ms       9.3min   │    │
│  │ UPDATE users SET updated_at..  8,234    12ms       1.6min   │    │
│  │ INSERT INTO events VALUES...   45,678   2ms        1.5min   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Backups (`/databases/[id]/backups`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ← production-db  /  Backups                                        │
├─────────────────────────────────────────────────────────────────────┤
│  [Point-in-Time Recovery]                          [Create Backup]  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Point-in-Time Recovery                                             │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ Restore database to any point in the last 7 days            │    │
│  │                                                             │    │
│  │ Recovery Window: Jan 9, 2026 10:00 AM → Jan 16, 2026 now    │    │
│  │ [──────────────────────────────●]                           │    │
│  │                                                             │    │
│  │ Restore to: [Jan 15, 2026 ▼] [14:30:00]    [Restore →]      │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Scheduled Backups                                                  │
│  Daily at 03:00 UTC • Retention: 7 days              [Settings]     │
│                                                                     │
│  Backup History                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ ✓ Jan 16, 2026 03:00 AM   2.1 GB   Automatic   [Restore]    │    │
│  │ ✓ Jan 15, 2026 03:00 AM   2.0 GB   Automatic   [Restore]    │    │
│  │ ✓ Jan 14, 2026 15:30 PM   1.9 GB   Manual      [Restore]    │    │
│  │ ✓ Jan 14, 2026 03:00 AM   1.9 GB   Automatic   [Restore]    │    │
│  │ ✓ Jan 13, 2026 03:00 AM   1.8 GB   Automatic   [Restore]    │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Team Management (`/team/members`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  Team Members                                     [Invite Member]   │
├─────────────────────────────────────────────────────────────────────┤
│  [Search members...]           [Role ▼]  [Status ▼]                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Members (4)                                                        │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ [Avatar] John Doe                                           │    │
│  │          john@company.com                                   │    │
│  │          Owner • Joined Jan 1, 2026                [•••]    │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │ [Avatar] Jane Smith                                         │    │
│  │          jane@company.com                                   │    │
│  │          Admin • Joined Jan 5, 2026                [•••]    │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │ [Avatar] Bob Wilson                                         │    │
│  │          bob@company.com                                    │    │
│  │          Developer • Joined Jan 10, 2026           [•••]    │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │ [Avatar] Alice Chen                                         │    │
│  │          alice@company.com                                  │    │
│  │          Viewer • Joined Jan 12, 2026              [•••]    │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Pending Invitations (2)                                            │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ mike@company.com    Developer    Sent Jan 15    [Resend]    │    │
│  │ sara@company.com    Viewer       Sent Jan 14    [Resend]    │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Billing (`/settings/billing`)

```
┌─────────────────────────────────────────────────────────────────────┐
│  Billing                                                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Current Plan: Pro                               [Change Plan]      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ $29/month base + usage                                      │    │
│  │ • 1000 compute hours included                               │    │
│  │ • 50 GB storage included                                    │    │
│  │ • Automatic backups (30 day retention)                      │    │
│  │ • Team collaboration                                        │    │
│  │ • Priority support                                          │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Current Billing Period: Jan 1 - Jan 31, 2026                       │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                                                             │    │
│  │ Base Plan                                         $29.00    │    │
│  │ Compute Hours (456 of 1000 included)               $0.00    │    │
│  │ Storage (12.4 GB of 50 GB included)                $0.00    │    │
│  │ ─────────────────────────────────────────────────────────   │    │
│  │ Estimated Total                                   $29.00    │    │
│  │                                                             │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Usage This Month                                                   │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ Compute Hours     [████████████░░░░░░░░]  456 / 1000 hrs    │    │
│  │ Storage           [████░░░░░░░░░░░░░░░░]  12.4 / 50 GB      │    │
│  │ Data Transfer     [██░░░░░░░░░░░░░░░░░░]  8.2 / 100 GB      │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Payment Method                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ •••• •••• •••• 4242    Visa    Exp: 12/28        [Update]   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  Invoice History                                                    │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ Dec 2025    $29.00    Paid    [Download]                    │    │
│  │ Nov 2025    $29.00    Paid    [Download]                    │    │
│  │ Oct 2025    $29.00    Paid    [Download]                    │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. Key Components

### Connection String Widget

```tsx
interface ConnectionStringProps {
  host: string;
  port: number;
  database: string;
  username: string;
  password: string;
  sslMode: 'require' | 'prefer';
}

// Features:
// - Toggle password visibility
// - One-click copy for each field
// - Copy full connection string
// - Show in different formats: URI, psql, programming languages
// - SSL certificate download
```

### Real-Time Metrics Chart

```tsx
interface MetricsChartProps {
  metric: 'cpu' | 'memory' | 'connections' | 'qps' | 'latency';
  clusterId: string;
  timeRange: '1h' | '6h' | '24h' | '7d' | '30d';
  refreshInterval?: number; // WebSocket for real-time
}

// Features:
// - Line/area chart with gradients
// - Threshold indicators (scale-up, alert levels)
// - Hover tooltips with exact values
// - Time range selector
// - Auto-refresh via WebSocket
```

### Query Editor

```tsx
// Based on Monaco Editor
interface QueryEditorProps {
  clusterId: string;
  initialValue?: string;
  onExecute: (query: string) => void;
}

// Features:
// - SQL syntax highlighting
// - Autocomplete (tables, columns, functions)
// - Keyboard shortcuts (Cmd+Enter to run)
// - Format SQL button
// - Save queries
// - Query history
// - Multiple tabs
// - Explain plan visualization
```

### Log Viewer

```tsx
interface LogViewerProps {
  clusterId: string;
  logType: 'query' | 'error' | 'slow' | 'connection';
  filters: LogFilters;
}

// Features:
// - Real-time streaming via SSE
// - Syntax highlighting for SQL
// - Search and filter
// - Severity levels (ERROR, WARNING, INFO, DEBUG)
// - Export logs
// - Time range filter
```

---

## 4. Tech Stack

### Frontend Framework

| Component | Technology | Rationale |
|-----------|------------|-----------|
| Framework | **Next.js 15** (App Router) | Server Components, streaming, edge runtime |
| Styling | **Tailwind CSS 4** | Utility-first, consistent design |
| Components | **shadcn/ui** | Accessible, customizable, modern |
| State | **TanStack Query v5** | Server state, caching, mutations |
| Forms | **React Hook Form + Zod** | Type-safe validation |
| Charts | **Recharts** | Responsive, React-native |
| Editor | **Monaco Editor** | VS Code quality |
| Icons | **Lucide React** | Consistent icon set |
| Animations | **Framer Motion** | Smooth transitions |

### Real-Time Updates

| Feature | Technology |
|---------|------------|
| Metrics streaming | **WebSocket** via Socket.io or native WS |
| Log streaming | **Server-Sent Events (SSE)** |
| Notifications | **WebSocket** |
| Cluster status | **Polling** (fallback) + **WebSocket** |

### Deployment

| Component | Technology |
|-----------|------------|
| Hosting | **Vercel** (Edge) or **Fly.io** |
| CDN | **Cloudflare** |
| Analytics | **PostHog** (self-hosted option) |
| Error Tracking | **Sentry** |

---

## 5. API Design

### REST API Endpoints

```
Authentication
POST   /v1/auth/register          # Create account
POST   /v1/auth/login             # Login, get tokens
POST   /v1/auth/logout            # Invalidate tokens
POST   /v1/auth/refresh           # Refresh access token
POST   /v1/auth/forgot-password   # Request password reset
POST   /v1/auth/reset-password    # Reset with token

Organizations
GET    /v1/organizations          # List user's organizations
POST   /v1/organizations          # Create organization
GET    /v1/organizations/:id      # Get organization
PATCH  /v1/organizations/:id      # Update organization
DELETE /v1/organizations/:id      # Delete organization

Clusters
GET    /v1/clusters               # List clusters
POST   /v1/clusters               # Create cluster
GET    /v1/clusters/:id           # Get cluster details
PATCH  /v1/clusters/:id           # Update cluster (scale, config)
DELETE /v1/clusters/:id           # Delete cluster
POST   /v1/clusters/:id/wake      # Wake sleeping cluster
POST   /v1/clusters/:id/sleep     # Force sleep cluster

GET    /v1/clusters/:id/connection   # Get connection details
GET    /v1/clusters/:id/metrics      # Get metrics
GET    /v1/clusters/:id/logs         # Get logs (with filters)

Backups
GET    /v1/clusters/:id/backups       # List backups
POST   /v1/clusters/:id/backups       # Create manual backup
POST   /v1/clusters/:id/restore       # Restore from backup/PITR
DELETE /v1/clusters/:id/backups/:bid  # Delete backup

Teams
GET    /v1/organizations/:id/members     # List members
POST   /v1/organizations/:id/invitations # Invite member
PATCH  /v1/organizations/:id/members/:mid # Update role
DELETE /v1/organizations/:id/members/:mid # Remove member

API Keys
GET    /v1/api-keys               # List API keys
POST   /v1/api-keys               # Create API key
DELETE /v1/api-keys/:id           # Revoke API key

Billing
GET    /v1/billing/usage          # Current usage
GET    /v1/billing/invoices       # Invoice history
POST   /v1/billing/subscription   # Create/update subscription
```

### WebSocket Events

```typescript
// Client -> Server
interface ClientEvents {
  'subscribe:metrics': { clusterId: string; metrics: string[] };
  'unsubscribe:metrics': { clusterId: string };
  'subscribe:logs': { clusterId: string; logType: string };
  'unsubscribe:logs': { clusterId: string };
}

// Server -> Client
interface ServerEvents {
  'metrics:update': {
    clusterId: string;
    timestamp: number;
    cpu: number;
    memory: number;
    connections: number;
    qps: number;
  };
  'logs:entry': {
    clusterId: string;
    timestamp: number;
    level: string;
    message: string;
  };
  'cluster:status': {
    clusterId: string;
    status: 'running' | 'sleeping' | 'provisioning' | 'error';
  };
  'notification': {
    type: 'info' | 'warning' | 'error';
    title: string;
    message: string;
  };
}
```

---

## 6. Design System

### Colors (Dark Theme Default)

```css
:root {
  /* Background */
  --bg-primary: #0a0a0b;
  --bg-secondary: #111113;
  --bg-tertiary: #18181b;
  --bg-elevated: #1f1f23;

  /* Text */
  --text-primary: #fafafa;
  --text-secondary: #a1a1aa;
  --text-tertiary: #71717a;

  /* Accent */
  --accent-primary: #3b82f6;    /* Blue */
  --accent-success: #22c55e;    /* Green */
  --accent-warning: #f59e0b;    /* Amber */
  --accent-error: #ef4444;      /* Red */

  /* Status */
  --status-running: #22c55e;
  --status-sleeping: #71717a;
  --status-error: #ef4444;
  --status-provisioning: #3b82f6;

  /* Border */
  --border-default: #27272a;
  --border-hover: #3f3f46;
}
```

### Typography

```css
/* Font Stack */
--font-sans: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
--font-mono: 'JetBrains Mono', 'Fira Code', monospace;

/* Sizes */
--text-xs: 0.75rem;    /* 12px */
--text-sm: 0.875rem;   /* 14px */
--text-base: 1rem;     /* 16px */
--text-lg: 1.125rem;   /* 18px */
--text-xl: 1.25rem;    /* 20px */
--text-2xl: 1.5rem;    /* 24px */
--text-3xl: 1.875rem;  /* 30px */
```

### Spacing Scale

```css
--space-1: 0.25rem;   /* 4px */
--space-2: 0.5rem;    /* 8px */
--space-3: 0.75rem;   /* 12px */
--space-4: 1rem;      /* 16px */
--space-5: 1.25rem;   /* 20px */
--space-6: 1.5rem;    /* 24px */
--space-8: 2rem;      /* 32px */
--space-10: 2.5rem;   /* 40px */
--space-12: 3rem;     /* 48px */
```

---

## 7. Responsive Breakpoints

```css
/* Mobile first */
--breakpoint-sm: 640px;   /* Mobile landscape */
--breakpoint-md: 768px;   /* Tablet */
--breakpoint-lg: 1024px;  /* Desktop */
--breakpoint-xl: 1280px;  /* Large desktop */
--breakpoint-2xl: 1536px; /* Extra large */
```

### Mobile Adaptations

- Sidebar collapses to bottom navigation on mobile
- Cards stack vertically
- Tables become card lists
- Charts maintain aspect ratio but simplify
- Modals become full-screen sheets

---

## 8. Accessibility

- **WCAG 2.1 AA** compliance target
- Keyboard navigation for all interactions
- Focus indicators on interactive elements
- Screen reader announcements for status changes
- Reduced motion support
- Color contrast ratios ≥ 4.5:1
- Form labels and error messages
- Skip navigation links
