import * as React from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import {
  Database,
  Book,
  Code,
  Zap,
  Clock,
  Layers,
  GitBranch,
  Shield,
  Server,
  Terminal,
  Search,
  ChevronRight,
  ExternalLink,
  Copy,
  Check,
} from "lucide-react";
import howleropsLogo from "@/src/assets/howlerops-icon.png";
import { Button } from "@/components/ui/button";
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";

export const Route = createFileRoute("/docs")({
  component: DocsPage,
});

const docSections = [
  {
    title: "Getting Started",
    icon: Book,
    items: [
      { title: "Quick Start Guide", href: "#quick-start" },
      { title: "Creating Your First Cluster", href: "#first-cluster" },
      { title: "Connecting to Your Database", href: "#connecting" },
      { title: "Dashboard Overview", href: "#dashboard" },
    ],
  },
  {
    title: "Core Features",
    icon: Database,
    items: [
      { title: "Automatic Sharding", href: "#sharding" },
      { title: "Time-Series Tables", href: "#timeseries" },
      { title: "Columnar Storage", href: "#columnar" },
      { title: "Tiered Storage", href: "#tiered" },
    ],
  },
  {
    title: "Advanced Features",
    icon: Zap,
    items: [
      { title: "Vector Search & AI", href: "#vector" },
      { title: "Change Data Capture (CDC)", href: "#cdc" },
      { title: "Data Pipelines", href: "#pipelines" },
      { title: "Raft Consensus", href: "#raft" },
    ],
  },
  {
    title: "Administration",
    icon: Server,
    items: [
      { title: "Cluster Management", href: "#cluster-management" },
      { title: "Monitoring & Metrics", href: "#monitoring" },
      { title: "Backups & Recovery", href: "#backups" },
      { title: "Security & Access", href: "#security" },
    ],
  },
];

function CodeBlock({
  children,
  language: _language = "sql",
}: {
  children: string;
  language?: string;
}): React.JSX.Element {
  const [copied, setCopied] = React.useState(false);

  const handleCopy = () => {
    navigator.clipboard.writeText(children);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <div className="relative group">
      <pre className="bg-muted rounded-lg p-4 overflow-x-auto font-mono text-sm">
        <code>{children}</code>
      </pre>
      <Button
        size="sm"
        variant="ghost"
        className="absolute top-2 right-2 opacity-0 group-hover:opacity-100 transition-opacity"
        onClick={handleCopy}
      >
        {copied ? (
          <Check className="h-4 w-4" />
        ) : (
          <Copy className="h-4 w-4" />
        )}
      </Button>
    </div>
  );
}

function DocsPage(): React.JSX.Element {
  return (
    <div className="min-h-screen bg-background">
      {/* Navigation */}
      <nav className="border-b bg-background/95 backdrop-blur supports-[backdrop-filter]:bg-background/60 sticky top-0 z-50">
        <div className="container mx-auto flex h-16 items-center justify-between px-4">
          <Link to="/" className="flex items-center gap-2">
            <img src={howleropsLogo} alt="HowlerOps" className="h-10 w-10" />
            <span className="text-xl font-bold">HowlerOps</span>
          </Link>
          <div className="hidden md:flex items-center gap-6">
            <Link
              to="/"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Features
            </Link>
            <Link
              to="/pricing"
              className="text-sm font-medium text-muted-foreground hover:text-foreground transition-colors"
            >
              Pricing
            </Link>
            <Link
              to="/docs"
              className="text-sm font-medium text-foreground"
            >
              Documentation
            </Link>
          </div>
          <div className="flex items-center gap-4">
            <Button variant="ghost" asChild>
              <Link to="/login">Sign In</Link>
            </Button>
            <Button asChild>
              <Link to="/register">Get Started</Link>
            </Button>
          </div>
        </div>
      </nav>

      <div className="container mx-auto px-4 py-8">
        <div className="flex gap-8">
          {/* Sidebar */}
          <aside className="hidden lg:block w-64 flex-shrink-0">
            <div className="sticky top-24 space-y-6">
              <div className="relative">
                <Search className="absolute left-3 top-1/2 -translate-y-1/2 h-4 w-4 text-muted-foreground" />
                <input
                  type="text"
                  placeholder="Search docs..."
                  className="w-full pl-10 pr-4 py-2 border rounded-md bg-background text-sm"
                />
              </div>
              <nav className="space-y-6">
                {docSections.map((section) => (
                  <div key={section.title}>
                    <h4 className="flex items-center gap-2 font-semibold text-sm mb-2">
                      <section.icon className="h-4 w-4" />
                      {section.title}
                    </h4>
                    <ul className="space-y-1 pl-6">
                      {section.items.map((item) => (
                        <li key={item.title}>
                          <a
                            href={item.href}
                            className="text-sm text-muted-foreground hover:text-foreground transition-colors block py-1"
                          >
                            {item.title}
                          </a>
                        </li>
                      ))}
                    </ul>
                  </div>
                ))}
              </nav>
            </div>
          </aside>

          {/* Main Content */}
          <main className="flex-1 max-w-4xl">
            {/* Header */}
            <div className="mb-12">
              <h1 className="text-4xl font-bold tracking-tight mb-4">
                OrochiDB Documentation
              </h1>
              <p className="text-xl text-muted-foreground">
                Learn how to use OrochiDB's powerful PostgreSQL HTAP features
                including automatic sharding, time-series optimization, columnar
                storage, and more.
              </p>
            </div>

            {/* Quick Links */}
            <div className="grid gap-4 md:grid-cols-2 mb-12">
              <Card className="hover:shadow-md transition-shadow">
                <a href="#quick-start">
                  <CardHeader>
                    <CardTitle className="flex items-center gap-2">
                      <Terminal className="h-5 w-5 text-primary" />
                      Quick Start
                    </CardTitle>
                    <CardDescription>
                      Get up and running in under 5 minutes
                    </CardDescription>
                  </CardHeader>
                </a>
              </Card>
              <Card className="hover:shadow-md transition-shadow">
                <a href="#sharding">
                  <CardHeader>
                    <CardTitle className="flex items-center gap-2">
                      <Database className="h-5 w-5 text-primary" />
                      Sharding Guide
                    </CardTitle>
                    <CardDescription>
                      Learn about automatic horizontal sharding
                    </CardDescription>
                  </CardHeader>
                </a>
              </Card>
              <Card className="hover:shadow-md transition-shadow">
                <a href="#timeseries">
                  <CardHeader>
                    <CardTitle className="flex items-center gap-2">
                      <Clock className="h-5 w-5 text-primary" />
                      Time-Series
                    </CardTitle>
                    <CardDescription>
                      Optimize your time-series workloads
                    </CardDescription>
                  </CardHeader>
                </a>
              </Card>
              <Card className="hover:shadow-md transition-shadow">
                <a href="#columnar">
                  <CardHeader>
                    <CardTitle className="flex items-center gap-2">
                      <Layers className="h-5 w-5 text-primary" />
                      Columnar Storage
                    </CardTitle>
                    <CardDescription>
                      Accelerate analytics with columnar format
                    </CardDescription>
                  </CardHeader>
                </a>
              </Card>
            </div>

            {/* Quick Start Section */}
            <section id="quick-start" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Book className="h-6 w-6" />
                Quick Start Guide
              </h2>
              <p className="text-muted-foreground mb-6">
                Get started with OrochiDB on HowlerOps in just a few steps.
              </p>

              <div className="space-y-6">
                <div>
                  <h3 className="text-lg font-semibold mb-3">
                    1. Create an Account
                  </h3>
                  <p className="text-muted-foreground mb-3">
                    Sign up for a free HowlerOps account at{" "}
                    <Link to="/register" className="text-primary hover:underline">
                      howlerops.com/register
                    </Link>
                    . No credit card required.
                  </p>
                </div>

                <div>
                  <h3 className="text-lg font-semibold mb-3">
                    2. Create Your First Cluster
                  </h3>
                  <p className="text-muted-foreground mb-3">
                    From the dashboard, click "New Cluster" and configure:
                  </p>
                  <ul className="list-disc pl-6 text-muted-foreground space-y-1">
                    <li>Choose a name for your cluster</li>
                    <li>Select your cloud provider (AWS, GCP, or Azure)</li>
                    <li>Pick a region close to your users</li>
                    <li>Choose your cluster tier (Free, Standard, or Professional)</li>
                  </ul>
                </div>

                <div id="connecting">
                  <h3 className="text-lg font-semibold mb-3">
                    3. Connect to Your Database
                  </h3>
                  <p className="text-muted-foreground mb-3">
                    Use your connection string to connect with any PostgreSQL client:
                  </p>
                  <CodeBlock language="bash">
{`# Using psql
psql "postgresql://username:password@your-cluster.howlerops.com:5432/dbname"

# Using Node.js
const { Pool } = require('pg');
const pool = new Pool({
  connectionString: 'postgresql://username:password@your-cluster.howlerops.com:5432/dbname'
});

# Using Python
import psycopg2
conn = psycopg2.connect("postgresql://username:password@your-cluster.howlerops.com:5432/dbname")`}
                  </CodeBlock>
                </div>
              </div>
            </section>

            {/* Sharding Section */}
            <section id="sharding" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Database className="h-6 w-6" />
                Automatic Sharding
              </h2>
              <p className="text-muted-foreground mb-6">
                OrochiDB automatically distributes your data across multiple
                nodes using hash-based sharding. This allows you to scale
                horizontally without changing your application code.
              </p>

              <Tabs defaultValue="create" className="mb-6">
                <TabsList>
                  <TabsTrigger value="create">Create Distributed Table</TabsTrigger>
                  <TabsTrigger value="query">Query Distributed Data</TabsTrigger>
                  <TabsTrigger value="rebalance">Rebalancing</TabsTrigger>
                </TabsList>
                <TabsContent value="create" className="mt-4">
                  <CodeBlock>
{`-- Create a distributed table with automatic sharding
SELECT create_distributed_table('orders', 'customer_id');

-- Or specify the number of shards
SELECT create_distributed_table(
  'orders',
  'customer_id',
  shard_count => 32
);

-- Create a reference table (replicated to all nodes)
SELECT create_reference_table('countries');`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="query" className="mt-4">
                  <CodeBlock>
{`-- Queries work exactly like regular PostgreSQL
SELECT customer_id, SUM(total)
FROM orders
WHERE created_at > NOW() - INTERVAL '30 days'
GROUP BY customer_id
ORDER BY SUM(total) DESC
LIMIT 10;

-- Joins between distributed tables
SELECT c.name, COUNT(o.id) as order_count
FROM customers c
JOIN orders o ON c.id = o.customer_id
GROUP BY c.name;`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="rebalance" className="mt-4">
                  <CodeBlock>
{`-- Check shard distribution
SELECT * FROM orochi_shard_stats();

-- Rebalance shards across nodes
SELECT rebalance_table_shards('orders');

-- Move specific shard
SELECT orochi_move_shard(
  shard_id => 102089,
  source_node => 'node1',
  target_node => 'node2'
);`}
                  </CodeBlock>
                </TabsContent>
              </Tabs>

              <Card>
                <CardHeader>
                  <CardTitle className="text-base">Best Practices</CardTitle>
                </CardHeader>
                <CardContent className="space-y-2 text-sm text-muted-foreground">
                  <p>
                    <strong>Choose your distribution column wisely:</strong> Pick a
                    column with high cardinality that's frequently used in WHERE
                    clauses and JOINs.
                  </p>
                  <p>
                    <strong>Use reference tables for lookup data:</strong> Small
                    tables that are frequently joined should be reference tables.
                  </p>
                  <p>
                    <strong>Co-locate related tables:</strong> Tables that are
                    frequently joined should use the same distribution column.
                  </p>
                </CardContent>
              </Card>
            </section>

            {/* Time-Series Section */}
            <section id="timeseries" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Clock className="h-6 w-6" />
                Time-Series Optimization
              </h2>
              <p className="text-muted-foreground mb-6">
                OrochiDB automatically partitions time-series data into chunks
                for optimal query performance and data lifecycle management.
              </p>

              <Tabs defaultValue="create" className="mb-6">
                <TabsList>
                  <TabsTrigger value="create">Create Hypertable</TabsTrigger>
                  <TabsTrigger value="compress">Compression</TabsTrigger>
                  <TabsTrigger value="aggregates">Continuous Aggregates</TabsTrigger>
                </TabsList>
                <TabsContent value="create" className="mt-4">
                  <CodeBlock>
{`-- Create a regular table
CREATE TABLE sensor_data (
  time        TIMESTAMPTZ NOT NULL,
  sensor_id   INTEGER,
  temperature DOUBLE PRECISION,
  humidity    DOUBLE PRECISION
);

-- Convert to hypertable with 1-day chunks
SELECT create_hypertable(
  'sensor_data',
  'time',
  chunk_time_interval => INTERVAL '1 day'
);

-- Create distributed hypertable
SELECT create_distributed_hypertable(
  'sensor_data',
  'time',
  'sensor_id',
  chunk_time_interval => INTERVAL '1 day'
);`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="compress" className="mt-4">
                  <CodeBlock>
{`-- Enable compression on hypertable
ALTER TABLE sensor_data SET (
  orochi.compression_enabled = true,
  orochi.compression_type = 'zstd',
  orochi.compress_after = INTERVAL '7 days'
);

-- Manually compress chunks
SELECT compress_chunk(
  chunk => '_timescaledb_internal._hyper_1_1_chunk'
);

-- View compression statistics
SELECT * FROM hypertable_compression_stats('sensor_data');`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="aggregates" className="mt-4">
                  <CodeBlock>
{`-- Create continuous aggregate for hourly rollups
CREATE MATERIALIZED VIEW sensor_hourly
WITH (continuous) AS
SELECT
  time_bucket('1 hour', time) AS bucket,
  sensor_id,
  AVG(temperature) AS avg_temp,
  MAX(temperature) AS max_temp,
  MIN(temperature) AS min_temp
FROM sensor_data
GROUP BY bucket, sensor_id;

-- Add refresh policy
SELECT add_continuous_aggregate_policy(
  'sensor_hourly',
  start_offset => INTERVAL '3 hours',
  end_offset => INTERVAL '1 hour',
  schedule_interval => INTERVAL '1 hour'
);`}
                  </CodeBlock>
                </TabsContent>
              </Tabs>
            </section>

            {/* Columnar Storage Section */}
            <section id="columnar" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Layers className="h-6 w-6" />
                Columnar Storage
              </h2>
              <p className="text-muted-foreground mb-6">
                Store analytical data in column-oriented format with advanced
                compression for 10x+ faster analytics queries and up to 90%
                storage savings.
              </p>

              <Tabs defaultValue="enable" className="mb-6">
                <TabsList>
                  <TabsTrigger value="enable">Enable Columnar</TabsTrigger>
                  <TabsTrigger value="compression">Compression Types</TabsTrigger>
                  <TabsTrigger value="hybrid">Hybrid Tables</TabsTrigger>
                </TabsList>
                <TabsContent value="enable" className="mt-4">
                  <CodeBlock>
{`-- Convert existing table to columnar storage
ALTER TABLE analytics_events
SET ACCESS METHOD columnar;

-- Create new columnar table
CREATE TABLE events (
  id BIGINT,
  event_type TEXT,
  payload JSONB,
  created_at TIMESTAMPTZ
) USING columnar;

-- Set compression options
ALTER TABLE events SET (
  columnar.compression = zstd,
  columnar.stripe_row_limit = 150000,
  columnar.chunk_group_row_limit = 10000
);`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="compression" className="mt-4">
                  <CodeBlock>
{`-- Available compression types
-- none: No compression (fastest writes)
-- lz4: Fast compression, moderate ratio
-- zstd: Best compression ratio (default)
-- delta: For sorted numeric columns
-- gorilla: For floating-point time-series
-- dictionary: For low-cardinality columns
-- rle: Run-length encoding for repeated values

-- Set per-column compression
ALTER TABLE events ALTER COLUMN event_type
  SET (compression = dictionary);

ALTER TABLE events ALTER COLUMN created_at
  SET (compression = delta);`}
                  </CodeBlock>
                </TabsContent>
                <TabsContent value="hybrid" className="mt-4">
                  <CodeBlock>
{`-- Create hybrid table (row + columnar partitions)
CREATE TABLE orders (
  id BIGINT,
  customer_id INT,
  total DECIMAL,
  created_at TIMESTAMPTZ
) PARTITION BY RANGE (created_at);

-- Recent data in row format for fast writes
CREATE TABLE orders_current PARTITION OF orders
  FOR VALUES FROM ('2024-01-01') TO (MAXVALUE);

-- Historical data in columnar for analytics
CREATE TABLE orders_historical PARTITION OF orders
  FOR VALUES FROM (MINVALUE) TO ('2024-01-01')
  USING columnar;`}
                  </CodeBlock>
                </TabsContent>
              </Tabs>
            </section>

            {/* Tiered Storage Section */}
            <section id="tiered" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Server className="h-6 w-6" />
                Tiered Storage
              </h2>
              <p className="text-muted-foreground mb-6">
                Automatically move data between storage tiers based on age and
                access patterns. Reduce costs while maintaining query access to
                all your data.
              </p>

              <CodeBlock>
{`-- Configure tiered storage policy
SELECT set_tiered_storage_policy(
  'events',
  hot_to_warm_after => INTERVAL '7 days',
  warm_to_cold_after => INTERVAL '30 days',
  cold_to_frozen_after => INTERVAL '90 days'
);

-- Hot tier: NVMe SSD (fastest, most expensive)
-- Warm tier: SSD (balanced performance/cost)
-- Cold tier: HDD (cost-effective storage)
-- Frozen tier: S3/Object storage (lowest cost)

-- View tier distribution
SELECT * FROM orochi_tier_stats('events');

-- Query across all tiers transparently
SELECT * FROM events
WHERE created_at > NOW() - INTERVAL '1 year';`}
              </CodeBlock>
            </section>

            {/* Vector Search Section */}
            <section id="vector" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Zap className="h-6 w-6" />
                Vector Search & AI
              </h2>
              <p className="text-muted-foreground mb-6">
                Build AI-powered applications with native vector storage and
                SIMD-optimized similarity search operations.
              </p>

              <CodeBlock>
{`-- Create table with vector column
CREATE TABLE documents (
  id SERIAL PRIMARY KEY,
  title TEXT,
  content TEXT,
  embedding vector(1536)  -- OpenAI ada-002 dimensions
);

-- Create HNSW index for fast similarity search
CREATE INDEX ON documents
USING hnsw (embedding vector_cosine_ops)
WITH (m = 16, ef_construction = 64);

-- Insert embeddings from your AI model
INSERT INTO documents (title, content, embedding)
VALUES ('My Document', 'Content here...', '[0.1, 0.2, ...]');

-- Find similar documents
SELECT title, content,
       1 - (embedding <=> query_embedding) AS similarity
FROM documents
ORDER BY embedding <=> query_embedding
LIMIT 10;

-- Hybrid search with filters
SELECT * FROM documents
WHERE category = 'tech'
ORDER BY embedding <=> query_embedding
LIMIT 10;`}
              </CodeBlock>
            </section>

            {/* CDC Section */}
            <section id="cdc" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <GitBranch className="h-6 w-6" />
                Change Data Capture (CDC)
              </h2>
              <p className="text-muted-foreground mb-6">
                Stream database changes to external systems in real-time. Perfect
                for event-driven architectures, data lakes, and search indexes.
              </p>

              <CodeBlock>
{`-- Create CDC subscription
SELECT create_cdc_subscription(
  'orders_to_kafka',
  source_tables => ARRAY['orders', 'order_items'],
  destination => 'kafka://broker:9092/orders-topic',
  format => 'avro'
);

-- Configure CDC options
ALTER SUBSCRIPTION orders_to_kafka SET (
  include_transaction_ids => true,
  include_timestamps => true,
  batch_size => 1000
);

-- Monitor CDC lag
SELECT * FROM cdc_subscription_stats('orders_to_kafka');

-- Pause/resume subscription
SELECT pause_cdc_subscription('orders_to_kafka');
SELECT resume_cdc_subscription('orders_to_kafka');`}
              </CodeBlock>
            </section>

            {/* Pipelines Section */}
            <section id="pipelines" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Code className="h-6 w-6" />
                Data Pipelines
              </h2>
              <p className="text-muted-foreground mb-6">
                Ingest data from Kafka, S3, and other sources directly into your
                database with built-in transformation capabilities.
              </p>

              <CodeBlock>
{`-- Create Kafka ingestion pipeline
SELECT create_pipeline(
  'kafka_events',
  source => 'kafka://broker:9092/events',
  target_table => 'events',
  format => 'json',
  transform => $$
    SELECT
      data->>'id' AS id,
      data->>'type' AS event_type,
      (data->>'timestamp')::timestamptz AS created_at
    FROM source_data
  $$
);

-- Create S3 ingestion pipeline
SELECT create_pipeline(
  's3_logs',
  source => 's3://my-bucket/logs/*.parquet',
  target_table => 'access_logs',
  format => 'parquet',
  schedule => 'every 1 hour'
);

-- Monitor pipeline status
SELECT * FROM pipeline_stats();`}
              </CodeBlock>
            </section>

            {/* Security Section */}
            <section id="security" className="mb-16 scroll-mt-24">
              <h2 className="text-2xl font-bold tracking-tight mb-4 flex items-center gap-2">
                <Shield className="h-6 w-6" />
                Security & Access Control
              </h2>
              <p className="text-muted-foreground mb-6">
                Enterprise-grade security with encryption, role-based access
                control, and audit logging.
              </p>

              <div className="space-y-4 text-muted-foreground">
                <div>
                  <h4 className="font-semibold text-foreground">Encryption</h4>
                  <ul className="list-disc pl-6 space-y-1">
                    <li>TLS 1.3 for data in transit</li>
                    <li>AES-256 encryption at rest</li>
                    <li>Customer-managed encryption keys (Enterprise)</li>
                  </ul>
                </div>
                <div>
                  <h4 className="font-semibold text-foreground">Authentication</h4>
                  <ul className="list-disc pl-6 space-y-1">
                    <li>Database username/password</li>
                    <li>JWT token authentication</li>
                    <li>SAML/SSO integration (Enterprise)</li>
                    <li>IP allowlisting</li>
                  </ul>
                </div>
                <div>
                  <h4 className="font-semibold text-foreground">Compliance</h4>
                  <ul className="list-disc pl-6 space-y-1">
                    <li>SOC 2 Type II certified</li>
                    <li>GDPR compliant</li>
                    <li>HIPAA compliant (Enterprise)</li>
                    <li>Full audit logging</li>
                  </ul>
                </div>
              </div>
            </section>

            {/* Next Steps */}
            <section className="border-t pt-12">
              <h2 className="text-2xl font-bold tracking-tight mb-6">
                Next Steps
              </h2>
              <div className="grid gap-4 md:grid-cols-2">
                <Card className="hover:shadow-md transition-shadow">
                  <Link to="/register">
                    <CardHeader>
                      <CardTitle className="flex items-center gap-2">
                        Create Your First Cluster
                        <ChevronRight className="h-4 w-4" />
                      </CardTitle>
                      <CardDescription>
                        Sign up and deploy a database in minutes
                      </CardDescription>
                    </CardHeader>
                  </Link>
                </Card>
                <Card className="hover:shadow-md transition-shadow">
                  <a
                    href="https://github.com/orochi-db/orochi"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <CardHeader>
                      <CardTitle className="flex items-center gap-2">
                        View on GitHub
                        <ExternalLink className="h-4 w-4" />
                      </CardTitle>
                      <CardDescription>
                        Explore the open-source PostgreSQL extension
                      </CardDescription>
                    </CardHeader>
                  </a>
                </Card>
              </div>
            </section>
          </main>
        </div>
      </div>

      {/* Footer */}
      <footer className="border-t py-12 mt-12">
        <div className="container mx-auto px-4">
          <div className="grid gap-8 md:grid-cols-4">
            <div>
              <div className="flex items-center gap-2 mb-4">
                <img src={howleropsLogo} alt="HowlerOps" className="h-8 w-8" />
                <span className="font-bold">HowlerOps</span>
              </div>
              <p className="text-sm text-muted-foreground">
                OrochiDB - The PostgreSQL platform for modern HTAP workloads.
              </p>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Product</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/" className="hover:text-foreground">
                    Features
                  </Link>
                </li>
                <li>
                  <Link to="/pricing" className="hover:text-foreground">
                    Pricing
                  </Link>
                </li>
                <li>
                  <Link to="/docs" className="hover:text-foreground">
                    Documentation
                  </Link>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Company</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <a href="#" className="hover:text-foreground">
                    About
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Blog
                  </a>
                </li>
                <li>
                  <a href="#" className="hover:text-foreground">
                    Careers
                  </a>
                </li>
              </ul>
            </div>
            <div>
              <h4 className="font-semibold mb-4">Legal</h4>
              <ul className="space-y-2 text-sm text-muted-foreground">
                <li>
                  <Link to="/privacy" className="hover:text-foreground">
                    Privacy Policy
                  </Link>
                </li>
                <li>
                  <Link to="/terms" className="hover:text-foreground">
                    Terms of Service
                  </Link>
                </li>
              </ul>
            </div>
          </div>
          <div className="border-t mt-8 pt-8 text-center text-sm text-muted-foreground">
            <p>
              &copy; {new Date().getFullYear()} HowlerOps. All rights
              reserved.
            </p>
          </div>
        </div>
      </footer>
    </div>
  );
}
