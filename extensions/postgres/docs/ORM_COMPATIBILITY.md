# Orochi DB ORM Compatibility Guide

## Executive Summary

This document analyzes ORM and database library compatibility with Orochi DB across TypeScript, Go, and Python ecosystems, and provides recommendations for building custom SDK/ORM solutions.

## Quick Recommendations

| Language | Top Choice | Runner-Up | Avoid |
|----------|-----------|-----------|-------|
| **TypeScript** | Drizzle ORM | Kysely | TypeORM |
| **Go** | pgx + sqlc | Bun | Ent |
| **Python** | SQLAlchemy 2.0 | psycopg3 | - |

---

## TypeScript/JavaScript ORMs

### 1. Drizzle ORM (Recommended)

**Score: 9/10 for Orochi DB compatibility**

**Strengths:**
- SQL-first design philosophy
- Excellent custom type support via `customType()`
- Magic `sql` template tag for raw SQL anywhere
- Migration system supports custom DDL

**Custom Type Example:**
```typescript
import { customType } from 'drizzle-orm/pg-core';

const vector = customType<{ data: number[]; driverData: string }>({
  dataType() { return 'vector(1536)'; },
  toDriver(value: number[]): string {
    return `[${value.join(',')}]`;
  },
  fromDriver(value: string): number[] {
    return JSON.parse(value);
  },
});
```

**Orochi Functions:**
```typescript
import { sql } from 'drizzle-orm';

// Create hypertable
await db.execute(sql`
  SELECT create_hypertable('sensor_data', 'timestamp',
    chunk_time_interval => INTERVAL '1 day')
`);

// Time bucket query
const results = await db.execute(sql`
  SELECT time_bucket('1 hour', timestamp) AS bucket,
         avg(value) AS avg_value
  FROM sensor_data
  GROUP BY bucket
`);
```

### 2. Kysely (Second Choice)

**Score: 8.5/10**

**Strengths:**
- Type-safe SQL query builder (not ORM)
- Minimal abstraction over SQL
- Excellent extensibility for custom helpers

**Custom Helper Example:**
```typescript
import { sql, RawBuilder } from 'kysely';

function timeBucket(interval: string, column: string): RawBuilder<Date> {
  return sql<Date>`time_bucket(${sql.lit(interval)}, ${sql.ref(column)})`;
}

const query = db
  .selectFrom('sensor_data')
  .select([timeBucket('1 hour', 'timestamp').as('bucket')])
  .groupBy('bucket');
```

### 3. Prisma (With Caveats)

**Score: 7/10**

**Limitations:**
- Custom types represented as `Unsupported` - no direct Prisma Client access
- Vector type requires raw SQL workarounds
- Models with `Unsupported` fields cannot use `create`/`update`

**Workaround:**
```typescript
// Insert via raw SQL
await prisma.$executeRaw`
  INSERT INTO embeddings (content, embedding)
  VALUES (${content}, ${embedding}::vector)
`;
```

### 4. TypeORM (Not Recommended)

**Score: 5/10**

- Custom type validation errors for extension types
- Throws `DataTypeNotSupportedError` for unknown types
- Workarounds may cause data loss on sync

---

## Go Libraries

### 1. pgx + sqlc (Recommended)

**pgx Score: 5/5 | sqlc Score: 5/5**

**Why this combination:**
- `sqlc` generates type-safe code for regular queries
- `pgx` provides full control for Orochi-specific operations
- Both share the same pgxpool connection

**Custom Type Registration:**
```go
config.AfterConnect = func(ctx context.Context, conn *pgx.Conn) error {
    vectorType, err := conn.LoadType(ctx, "orochi.vector")
    if err != nil { return err }
    conn.TypeMap().RegisterType(vectorType)
    return nil
}
```

**sqlc Query:**
```sql
-- name: GetTimeBucketedMetrics :many
SELECT
    time_bucket($1::interval, timestamp) AS bucket,
    avg(value) AS avg_value
FROM metrics
WHERE timestamp >= $2 AND timestamp < $3
GROUP BY bucket;
```

### 2. Bun (Alternative)

**Score: 4/5**

- SQL-first ORM with minimal overhead
- Good PostgreSQL support
- Built-in OpenTelemetry tracing

### 3. GORM (With Limitations)

**Score: 3.5/5**

- Full-featured but abstraction overhead
- Custom types via Scanner/Valuer interfaces
- Use `db.Raw()` for all Orochi-specific operations

---

## Python Libraries

### 1. SQLAlchemy 2.0 (Recommended)

**Score: Excellent**

**Strengths:**
- Full custom type support via `UserDefinedType`
- Alembic migrations with extension support
- PostgreSQL dialect includes JSONB, arrays, ranges

**Custom Vector Type:**
```python
from sqlalchemy.types import UserDefinedType

class OrochiVector(UserDefinedType):
    cache_ok = True

    def __init__(self, dim=None):
        self.dim = dim

    def get_col_spec(self):
        return f"vector({self.dim})" if self.dim else "vector"

    def bind_processor(self, dialect):
        def process(value):
            return f"[{','.join(map(str, value))}]" if value else None
        return process
```

**Orochi Operations:**
```python
from sqlalchemy import text

with engine.connect() as conn:
    conn.execute(text("""
        SELECT create_hypertable('metrics', 'timestamp',
            chunk_time_interval => INTERVAL '1 day')
    """))
```

### 2. psycopg3 (High Performance)

**Score: Excellent**

- Native async support with connection pooling
- Row Factories for Pydantic model mapping
- Full control over type adaptation

### 3. asyncpg (Maximum Performance)

**Score: Good**

- 5x faster than psycopg3 in benchmarks
- Custom codec system for type extensions
- Non-standard API (not DB-API 2.0 compliant)

---

## Building Custom ORM/SDK

### Recommendation: SDK + ORM Extensions

**Phase 1: Core SDK (3 months)**
Build lightweight SDKs providing:
- Type-safe function wrappers for Orochi SQL functions
- DSL builders for Pipelines, Workflows, CDC, Streams, Tasks
- Query helpers for time_bucket, vector operations

**Phase 2: ORM Extensions (2 months each)**
Build extensions for popular ORMs:
- TypeScript: Prisma generator + Drizzle dialect
- Python: SQLAlchemy dialect + Django backend
- Go: GORM driver hooks

### Effort Estimates

| Approach | TypeScript | Python | Go |
|----------|-----------|--------|-----|
| **Full ORM** | ~74,000 LOC | ~60,000 LOC | ~73,000 LOC |
| **SDK Only** | ~23,000 LOC | ~18,000 LOC | ~23,000 LOC |
| **ORM Plugin** | ~8,000 LOC | ~7,000 LOC | ~5,000 LOC |

### SDK API Example (TypeScript)

```typescript
import { OrochiClient, Pipeline, Workflow } from '@orochi-db/sdk';

const orochi = new OrochiClient(pool);

// Distributed table
await orochi.createDistributedTable({
  table: 'orders',
  distributionColumn: 'customer_id',
  shardCount: 32
});

// Hypertable
await orochi.createHypertable({
  table: 'metrics',
  timeColumn: 'time',
  chunkInterval: '1 day'
});

// Pipeline DSL
const pipeline = Pipeline.create('kafka_events')
  .source('kafka', { bootstrapServers: 'kafka:9092', topic: 'events' })
  .transform({ event_time: 'timestamp', data: 'jsonb' })
  .target('events')
  .build();

await orochi.createPipeline(pipeline);
```

---

## Integration Patterns

### Pattern 1: Hybrid Approach (Best Practice)

Use ORM for CRUD operations + raw SQL for Orochi-specific features:

```typescript
// orochi-client.ts
export const orochiClient = {
  db, // Drizzle/Prisma for CRUD

  async createHypertable(table: string, timeColumn: string) {
    return db.execute(sql`
      SELECT create_hypertable(${sql.identifier(table)}, ${timeColumn})
    `);
  },

  async timeBucketQuery(table: string, bucket: string) {
    return db.execute(sql`
      SELECT time_bucket(${bucket}::interval, timestamp) AS bucket,
             COUNT(*) as count
      FROM ${sql.identifier(table)}
      GROUP BY bucket
    `);
  }
};
```

### Pattern 2: Migration Strategy

```typescript
// migrations/001_setup_orochi.ts
export async function up(db) {
  await sql`CREATE EXTENSION IF NOT EXISTS orochi`.execute(db);

  await db.schema.createTable('sensor_data')
    .addColumn('timestamp', 'timestamptz')
    .addColumn('value', 'double precision')
    .execute();

  await sql`
    SELECT create_hypertable('sensor_data', 'timestamp')
  `.execute(db);
}
```

---

## Limitations Across All ORMs

1. **No native distributed table awareness** - Routing handled by Orochi at database level
2. **Custom type workarounds required** - `vector`, `hll`, `tdigest` need manual definition
3. **Time-series functions via raw SQL** - `time_bucket()` requires raw SQL
4. **Schema introspection limitations** - ORMs may not understand Orochi metadata tables

---

## Conclusion

For most Orochi DB use cases:

| Component | Recommendation |
|-----------|----------------|
| **TypeScript ORM** | Drizzle ORM |
| **Go ORM** | pgx + sqlc |
| **Python ORM** | SQLAlchemy 2.0 |
| **Driver** | Native PostgreSQL drivers |
| **Migrations** | Raw SQL for Orochi DDL |
| **Custom SDK** | Build SDK first, ORM plugins second |

The SDK approach provides the fastest time to value (3 months vs 12+ months for full ORM) with lower maintenance burden while maintaining complete feature coverage.
