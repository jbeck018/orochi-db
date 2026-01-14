/*-------------------------------------------------------------------------
 *
 * physical_sharding.c
 *    Orochi DB physical shard table management
 *
 * This module creates actual physical shard tables and sets up
 * transparent routing so users can query the parent table without
 * knowing about the underlying shards.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "nodes/nodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "../orochi.h"
#include "../core/catalog.h"
#include "distribution.h"
#include "physical_sharding.h"

/* ============================================================
 * Physical Shard Table Creation
 * ============================================================ */

/*
 * Generate the name for a shard table
 */
char *
orochi_get_shard_table_name(Oid table_oid, int shard_index)
{
    char *table_name = get_rel_name(table_oid);
    char *shard_name;

    if (table_name == NULL)
        return NULL;

    shard_name = psprintf("%s_shard_%d", table_name, shard_index);
    return shard_name;
}

/*
 * Get the OID of a physical shard table
 */
Oid
orochi_get_shard_table_oid(Oid parent_table_oid, int shard_index)
{
    char *shard_name;
    Oid namespace_oid;
    Oid shard_oid;

    shard_name = orochi_get_shard_table_name(parent_table_oid, shard_index);
    if (shard_name == NULL)
        return InvalidOid;

    namespace_oid = get_rel_namespace(parent_table_oid);
    shard_oid = get_relname_relid(shard_name, namespace_oid);

    pfree(shard_name);
    return shard_oid;
}

/*
 * Create physical shard tables for a distributed table
 */
void
orochi_create_physical_shards(Oid table_oid, int shard_count, const char *dist_column)
{
    StringInfoData query;
    char *schema_name;
    char *table_name;
    int i;
    int ret;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    if (schema_name == NULL || table_name == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("could not find table with OID %u", table_oid)));

    SPI_connect();

    /* Create each physical shard table as a copy of the parent structure */
    for (i = 0; i < shard_count; i++)
    {
        initStringInfo(&query);

        /* Create shard table with same structure using CREATE TABLE ... LIKE */
        appendStringInfo(&query,
            "CREATE TABLE IF NOT EXISTS %s.%s_shard_%d "
            "(LIKE %s.%s INCLUDING ALL)",
            quote_identifier(schema_name),
            table_name, i,
            quote_identifier(schema_name),
            quote_identifier(table_name));

        ret = SPI_execute(query.data, false, 0);
        if (ret != SPI_OK_UTILITY)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("failed to create shard table %s_shard_%d", table_name, i)));

        pfree(query.data);

        /* Add constraint to enforce hash range (for safety) */
        initStringInfo(&query);
        appendStringInfo(&query,
            "COMMENT ON TABLE %s.%s_shard_%d IS 'Orochi shard %d of %d for %s.%s, distribution column: %s'",
            quote_identifier(schema_name),
            table_name, i,
            i, shard_count,
            schema_name, table_name, dist_column);

        SPI_execute(query.data, false, 0);
        pfree(query.data);
    }

    SPI_finish();

    elog(LOG, "Created %d physical shard tables for %s.%s",
         shard_count, schema_name, table_name);
}

/*
 * Drop all physical shard tables for a distributed table
 */
void
orochi_drop_physical_shards(Oid table_oid)
{
    StringInfoData query;
    OrochiTableInfo *info;
    char *schema_name;
    char *table_name;
    int i;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || !info->is_distributed)
        return;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    SPI_connect();

    for (i = 0; i < info->shard_count; i++)
    {
        initStringInfo(&query);
        appendStringInfo(&query,
            "DROP TABLE IF EXISTS %s.%s_shard_%d CASCADE",
            quote_identifier(schema_name),
            table_name, i);

        SPI_execute(query.data, false, 0);
        pfree(query.data);
    }

    SPI_finish();

    elog(LOG, "Dropped %d physical shard tables for %s.%s",
         info->shard_count, schema_name, table_name);
}

/* ============================================================
 * Data Redistribution
 * ============================================================ */

/*
 * Move existing data from parent table to physical shards
 */
int64
orochi_redistribute_data(Oid table_oid)
{
    StringInfoData query;
    OrochiTableInfo *info;
    char *schema_name;
    char *table_name;
    char *dist_column;
    int64 total_moved = 0;
    int i;
    int ret;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || !info->is_distributed)
        return 0;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);
    dist_column = info->distribution_column;

    SPI_connect();

    /* Insert data into each shard based on hash of distribution column */
    for (i = 0; i < info->shard_count; i++)
    {
        int32 hash_min, hash_max;
        int hash_range = INT32_MAX / info->shard_count;

        hash_min = (i == 0) ? INT32_MIN : (INT32_MIN + (i * hash_range));
        hash_max = (i == info->shard_count - 1) ? INT32_MAX : (INT32_MIN + ((i + 1) * hash_range) - 1);

        initStringInfo(&query);

        /*
         * Use orochi.hash_value() to compute hash and filter rows for this shard.
         * The hash function is stable and matches what we use for routing.
         */
        appendStringInfo(&query,
            "INSERT INTO %s.%s_shard_%d "
            "SELECT * FROM %s.%s "
            "WHERE orochi.hash_value(%s) >= %d AND orochi.hash_value(%s) <= %d",
            quote_identifier(schema_name), table_name, i,
            quote_identifier(schema_name), quote_identifier(table_name),
            quote_identifier(dist_column), hash_min,
            quote_identifier(dist_column), hash_max);

        ret = SPI_execute(query.data, false, 0);
        if (ret == SPI_OK_INSERT)
            total_moved += SPI_processed;

        pfree(query.data);
    }

    /* Delete data from parent table after redistribution */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM %s.%s",
        quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(LOG, "Redistributed %ld rows from %s.%s to %d shards",
         total_moved, schema_name, table_name, info->shard_count);

    return total_moved;
}

/* ============================================================
 * Routing Trigger Functions
 * ============================================================ */

/*
 * Compute which shard a value belongs to
 */
int32
orochi_compute_shard_for_value(Datum value, Oid type_oid, int32 shard_count)
{
    int32 hash_value;

    hash_value = orochi_hash_datum(value, type_oid);
    return orochi_get_shard_index(hash_value, shard_count);
}

/*
 * Install INSERT routing trigger on parent table
 *
 * This creates:
 * 1. A trigger function that routes inserts to the correct shard
 * 2. A BEFORE INSERT trigger that calls the function and returns NULL
 * 3. INSTEAD OF triggers on the union view for full transparency
 */
void
orochi_install_routing_trigger(Oid table_oid, const char *dist_column, int32 shard_count)
{
    StringInfoData query;
    char *schema_name;
    char *table_name;
    int ret;

    if (shard_count <= 0)
        return;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    SPI_connect();

    /* Create the INSERT routing trigger function using E'' string to handle escaping */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE FUNCTION %s.%s_route_insert() "
        "RETURNS TRIGGER AS $orochi_trigger$ "
        "DECLARE "
        "    shard_idx INTEGER; "
        "BEGIN "
        "    shard_idx := orochi.get_shard_index(orochi.hash_value(NEW.%s), %d); "
        "    EXECUTE format('INSERT INTO %s.%s_shard_%%s VALUES ($1.*)', shard_idx) USING NEW; "
        "    RETURN NULL; "
        "END; "
        "$orochi_trigger$ LANGUAGE plpgsql",
        quote_identifier(schema_name),
        table_name,
        quote_identifier(dist_column),
        shard_count,
        schema_name,
        table_name);

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(WARNING, "Failed to create INSERT routing function for %s.%s", schema_name, table_name);
    pfree(query.data);

    /* Create DELETE routing function */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE FUNCTION %s.%s_route_delete() "
        "RETURNS TRIGGER AS $orochi_trigger$ "
        "DECLARE "
        "    shard_idx INTEGER; "
        "BEGIN "
        "    shard_idx := orochi.get_shard_index(orochi.hash_value(OLD.%s), %d); "
        "    EXECUTE format('DELETE FROM %s.%s_shard_%%s WHERE ctid IN (SELECT ctid FROM %s.%s_shard_%%s WHERE %s = $1 LIMIT 1)', shard_idx, shard_idx) USING OLD.%s; "
        "    RETURN NULL; "
        "END; "
        "$orochi_trigger$ LANGUAGE plpgsql",
        quote_identifier(schema_name), table_name,
        quote_identifier(dist_column), shard_count,
        schema_name, table_name, schema_name, table_name, dist_column, dist_column);

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(WARNING, "Failed to create DELETE routing function for %s.%s", schema_name, table_name);
    pfree(query.data);

    /* Create BEFORE INSERT trigger */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP TRIGGER IF EXISTS %s_insert_router ON %s.%s",
        table_name, quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER %s_insert_router "
        "BEFORE INSERT ON %s.%s "
        "FOR EACH ROW EXECUTE FUNCTION %s.%s_route_insert()",
        table_name,
        quote_identifier(schema_name), quote_identifier(table_name),
        quote_identifier(schema_name), table_name);

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(WARNING, "Failed to create INSERT trigger for %s.%s", schema_name, table_name);
    pfree(query.data);

    /* Create BEFORE DELETE trigger */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP TRIGGER IF EXISTS %s_delete_router ON %s.%s",
        table_name, quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER %s_delete_router "
        "BEFORE DELETE ON %s.%s "
        "FOR EACH ROW EXECUTE FUNCTION %s.%s_route_delete()",
        table_name,
        quote_identifier(schema_name), quote_identifier(table_name),
        quote_identifier(schema_name), table_name);

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(WARNING, "Failed to create DELETE trigger for %s.%s", schema_name, table_name);
    pfree(query.data);

    SPI_finish();

    elog(LOG, "Installed routing triggers on %s.%s", schema_name, table_name);
}

/*
 * Remove routing triggers from parent table
 */
void
orochi_remove_routing_trigger(Oid table_oid)
{
    StringInfoData query;
    char *schema_name;
    char *table_name;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    SPI_connect();

    /* Drop triggers */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP TRIGGER IF EXISTS %s_insert_router ON %s.%s",
        table_name, quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP TRIGGER IF EXISTS %s_update_router ON %s.%s",
        table_name, quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP TRIGGER IF EXISTS %s_delete_router ON %s.%s",
        table_name, quote_identifier(schema_name), quote_identifier(table_name));
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Drop functions */
    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP FUNCTION IF EXISTS %s.%s_route_insert()",
        quote_identifier(schema_name), table_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP FUNCTION IF EXISTS %s.%s_route_update()",
        quote_identifier(schema_name), table_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP FUNCTION IF EXISTS %s.%s_route_delete()",
        quote_identifier(schema_name), table_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(LOG, "Removed routing triggers from %s.%s", schema_name, table_name);
}

/* ============================================================
 * Union View Management
 * ============================================================ */

/*
 * Create a view that UNIONs all shard tables for transparent SELECT
 * Also creates INSTEAD OF triggers for INSERT/UPDATE/DELETE on the view
 */
void
orochi_create_union_view(Oid table_oid, const char *dist_column, int32 shard_count)
{
    StringInfoData query;
    char *schema_name;
    char *table_name;
    int i;
    int ret;

    if (shard_count <= 0 || dist_column == NULL)
        return;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    SPI_connect();

    /* Build UNION ALL view of all shards */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE VIEW %s.%s_all_shards AS ",
        quote_identifier(schema_name), table_name);

    for (i = 0; i < shard_count; i++)
    {
        if (i > 0)
            appendStringInfoString(&query, " UNION ALL ");
        appendStringInfo(&query,
            "SELECT * FROM %s.%s_shard_%d",
            quote_identifier(schema_name), table_name, i);
    }

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_UTILITY)
        elog(WARNING, "Failed to create union view for %s.%s", schema_name, table_name);
    pfree(query.data);

    /* Create INSTEAD OF INSERT trigger function for the view */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE FUNCTION %s.%s_view_insert() "
        "RETURNS TRIGGER AS $orochi_trigger$ "
        "DECLARE "
        "    shard_idx INTEGER; "
        "BEGIN "
        "    shard_idx := orochi.get_shard_index(orochi.hash_value(NEW.%s), %d); "
        "    EXECUTE format('INSERT INTO %s.%s_shard_%%s VALUES ($1.*)', shard_idx) USING NEW; "
        "    RETURN NEW; "
        "END; "
        "$orochi_trigger$ LANGUAGE plpgsql",
        quote_identifier(schema_name), table_name,
        quote_identifier(dist_column), shard_count,
        schema_name, table_name);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Create INSTEAD OF UPDATE trigger function - uses DELETE + INSERT strategy */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE FUNCTION %s.%s_view_update() "
        "RETURNS TRIGGER AS $orochi_trigger$ "
        "DECLARE "
        "    old_shard INTEGER; "
        "    new_shard INTEGER; "
        "BEGIN "
        "    old_shard := orochi.get_shard_index(orochi.hash_value(OLD.%s), %d); "
        "    new_shard := orochi.get_shard_index(orochi.hash_value(NEW.%s), %d); "
        "    EXECUTE format('DELETE FROM %s.%s_shard_%%s WHERE %s = $1', old_shard) USING OLD.%s; "
        "    EXECUTE format('INSERT INTO %s.%s_shard_%%s VALUES ($1.*)', new_shard) USING NEW; "
        "    RETURN NEW; "
        "END; "
        "$orochi_trigger$ LANGUAGE plpgsql",
        quote_identifier(schema_name), table_name,
        quote_identifier(dist_column), shard_count,
        quote_identifier(dist_column), shard_count,
        schema_name, table_name, dist_column, dist_column,
        schema_name, table_name);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Create INSTEAD OF DELETE trigger function */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE OR REPLACE FUNCTION %s.%s_view_delete() "
        "RETURNS TRIGGER AS $orochi_trigger$ "
        "DECLARE "
        "    shard_idx INTEGER; "
        "BEGIN "
        "    shard_idx := orochi.get_shard_index(orochi.hash_value(OLD.%s), %d); "
        "    EXECUTE format('DELETE FROM %s.%s_shard_%%s WHERE ctid = (SELECT ctid FROM %s.%s_shard_%%s WHERE %s = $1 LIMIT 1)', shard_idx, shard_idx) USING OLD.%s; "
        "    RETURN OLD; "
        "END; "
        "$orochi_trigger$ LANGUAGE plpgsql",
        quote_identifier(schema_name), table_name,
        quote_identifier(dist_column), shard_count,
        schema_name, table_name, schema_name, table_name, dist_column, dist_column);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Create INSTEAD OF INSERT trigger on view */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER %s_view_insert_trigger "
        "INSTEAD OF INSERT ON %s.%s_all_shards "
        "FOR EACH ROW EXECUTE FUNCTION %s.%s_view_insert()",
        table_name,
        quote_identifier(schema_name), table_name,
        quote_identifier(schema_name), table_name);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Create INSTEAD OF UPDATE trigger on view */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER %s_view_update_trigger "
        "INSTEAD OF UPDATE ON %s.%s_all_shards "
        "FOR EACH ROW EXECUTE FUNCTION %s.%s_view_update()",
        table_name,
        quote_identifier(schema_name), table_name,
        quote_identifier(schema_name), table_name);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    /* Create INSTEAD OF DELETE trigger on view */
    initStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER %s_view_delete_trigger "
        "INSTEAD OF DELETE ON %s.%s_all_shards "
        "FOR EACH ROW EXECUTE FUNCTION %s.%s_view_delete()",
        table_name,
        quote_identifier(schema_name), table_name,
        quote_identifier(schema_name), table_name);

    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();

    elog(LOG, "Created union view %s.%s_all_shards with INSTEAD OF triggers", schema_name, table_name);
}

/*
 * Drop the union view
 */
void
orochi_drop_union_view(Oid table_oid)
{
    StringInfoData query;
    char *schema_name;
    char *table_name;

    schema_name = get_namespace_name(get_rel_namespace(table_oid));
    table_name = get_rel_name(table_oid);

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "DROP VIEW IF EXISTS %s.%s_all_shards CASCADE",
        quote_identifier(schema_name), table_name);
    SPI_execute(query.data, false, 0);
    pfree(query.data);

    SPI_finish();
}

/* ============================================================
 * Query Rewriting Support
 * ============================================================ */

/*
 * Check if we should rewrite a query for this table
 */
bool
orochi_should_rewrite_query(Oid table_oid)
{
    OrochiTableInfo *info;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL)
        return false;

    return info->is_distributed && info->shard_count > 1;
}

/*
 * Get list of shard table OIDs that need to be queried
 * Based on WHERE clause analysis for shard pruning
 */
List *
orochi_get_target_shards_for_query(Oid table_oid, Node *quals)
{
    OrochiTableInfo *info;
    List *shard_oids = NIL;
    int i;

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || !info->is_distributed)
        return NIL;

    /* TODO: Implement actual shard pruning based on quals analysis
     * For now, return all shards */
    for (i = 0; i < info->shard_count; i++)
    {
        Oid shard_oid = orochi_get_shard_table_oid(table_oid, i);
        if (OidIsValid(shard_oid))
            shard_oids = lappend_oid(shard_oids, shard_oid);
    }

    return shard_oids;
}

/*
 * Expand a range table entry to multiple shard table entries
 * Returns a list of modified RangeTblEntry structures
 */
List *
orochi_expand_rte_to_shards(RangeTblEntry *rte, Node *quals)
{
    List *shard_rtes = NIL;
    List *shard_oids;
    ListCell *lc;

    if (rte->rtekind != RTE_RELATION)
        return NIL;

    shard_oids = orochi_get_target_shards_for_query(rte->relid, quals);

    foreach(lc, shard_oids)
    {
        Oid shard_oid = lfirst_oid(lc);
        RangeTblEntry *shard_rte;

        /* Create a copy of the RTE pointing to the shard table */
        shard_rte = (RangeTblEntry *) copyObjectImpl(rte);
        shard_rte->relid = shard_oid;
        shard_rte->alias = NULL;  /* Will be set by planner */

        shard_rtes = lappend(shard_rtes, shard_rte);
    }

    return shard_rtes;
}

/* ============================================================
 * SQL-Callable Functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(orochi_hash_value_sql);
PG_FUNCTION_INFO_V1(orochi_get_shard_index_sql);
PG_FUNCTION_INFO_V1(orochi_get_shard_for_value_sql);

/*
 * SQL function: orochi.hash_value(anyelement) -> integer
 * Computes a stable hash value for routing
 */
Datum
orochi_hash_value_sql(PG_FUNCTION_ARGS)
{
    Datum value = PG_GETARG_DATUM(0);
    Oid type_oid = get_fn_expr_argtype(fcinfo->flinfo, 0);
    int32 hash_value;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    hash_value = orochi_hash_datum(value, type_oid);

    PG_RETURN_INT32(hash_value);
}

/*
 * SQL function: orochi.get_shard_index(hash_value integer, shard_count integer) -> integer
 * Returns the shard index (0-based) for a hash value
 */
Datum
orochi_get_shard_index_sql(PG_FUNCTION_ARGS)
{
    int32 hash_value = PG_GETARG_INT32(0);
    int32 shard_count = PG_GETARG_INT32(1);
    int32 shard_index;

    if (shard_count <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("shard_count must be positive")));

    shard_index = orochi_get_shard_index(hash_value, shard_count);

    PG_RETURN_INT32(shard_index);
}

/*
 * SQL function: orochi.get_shard_for_value(table_oid oid, value anyelement) -> integer
 * Returns the shard index for a value in a specific table
 */
Datum
orochi_get_shard_for_value_sql(PG_FUNCTION_ARGS)
{
    Oid table_oid = PG_GETARG_OID(0);
    Datum value = PG_GETARG_DATUM(1);
    Oid type_oid = get_fn_expr_argtype(fcinfo->flinfo, 1);
    OrochiTableInfo *info;
    int32 hash_value;
    int32 shard_index;

    if (PG_ARGISNULL(1))
        PG_RETURN_NULL();

    info = orochi_catalog_get_table(table_oid);
    if (info == NULL || !info->is_distributed)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("table is not distributed")));

    hash_value = orochi_hash_datum(value, type_oid);
    shard_index = orochi_get_shard_index(hash_value, info->shard_count);

    PG_RETURN_INT32(shard_index);
}
