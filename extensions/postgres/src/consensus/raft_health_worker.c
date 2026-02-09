/*-------------------------------------------------------------------------
 * raft_health_worker.c
 *    Background worker for Raft cluster health checking
 *
 * This worker periodically checks the health of all nodes in the
 * Orochi cluster by connecting to each node and executing a simple
 * query. Results are stored in the orochi.orochi_node_health table
 * for monitoring and failure detection.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *-------------------------------------------------------------------------
 */

#ifdef RAFT_INTEGRATION

/* postgres.h must be included first */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "../orochi.h"
#include "raft.h"
#include "raft_integration.h"

/* GUC variables (defined in init.c) */
extern int orochi_health_check_interval;
extern int orochi_health_check_failure_threshold;

/* Signal handling */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* Forward declarations */
static void health_sighup_handler(SIGNAL_ARGS);
static void health_sigterm_handler(SIGNAL_ARGS);
static bool check_node_health(const char *connstr, int *latency_ms);
static void update_node_health(int node_id, bool is_healthy, int latency_ms);

/* ============================================================
 * Signal Handlers
 * ============================================================ */

static void
health_sighup_handler(SIGNAL_ARGS)
{
    int save_errno = errno;

    got_sighup = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
health_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;

    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/* ============================================================
 * Health Check Logic
 * ============================================================ */

/*
 * check_node_health
 *    Attempt to connect to a node and execute a ping query.
 *    Returns true if the node is healthy, false otherwise.
 *    Sets latency_ms to the round-trip time in milliseconds.
 */
static bool
check_node_health(const char *connstr, int *latency_ms)
{
    PGconn *conn;
    PGresult *res;
    TimestampTz start_time;
    TimestampTz end_time;
    long secs;
    int microsecs;

    *latency_ms = 0;

    start_time = GetCurrentTimestamp();

    conn = PQconnectdb(connstr);
    if (PQstatus(conn) != CONNECTION_OK)
    {
        elog(DEBUG1, "Health check: connection failed to %s: %s",
             connstr, PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }

    res = PQexec(conn, "SELECT 1");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        elog(DEBUG1, "Health check: query failed on %s: %s",
             connstr, PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);

    end_time = GetCurrentTimestamp();
    TimestampDifference(start_time, end_time, &secs, &microsecs);
    *latency_ms = (int)(secs * 1000 + microsecs / 1000);

    return true;
}

/*
 * update_node_health
 *    Update the orochi.orochi_node_health table via SPI with
 *    the health check results for a given node.
 */
static void
update_node_health(int node_id, bool is_healthy, int latency_ms)
{
    int ret;
    StringInfoData query;

    initStringInfo(&query);

    /*
     * Use INSERT ... ON CONFLICT to upsert the health record.
     * The orochi_node_health table is expected to have:
     *   node_id (PK), is_healthy, last_seen, latency_ms, consecutive_failures
     */
    appendStringInfo(&query,
                     "INSERT INTO orochi.orochi_node_health "
                     "(node_id, is_healthy, last_seen, latency_ms, consecutive_failures) "
                     "VALUES (%d, %s, now(), %d, "
                     "CASE WHEN %s THEN 0 ELSE 1 END) "
                     "ON CONFLICT (node_id) DO UPDATE SET "
                     "is_healthy = EXCLUDED.is_healthy, "
                     "last_seen = CASE WHEN EXCLUDED.is_healthy THEN now() "
                     "ELSE orochi.orochi_node_health.last_seen END, "
                     "latency_ms = EXCLUDED.latency_ms, "
                     "consecutive_failures = CASE WHEN EXCLUDED.is_healthy THEN 0 "
                     "ELSE orochi.orochi_node_health.consecutive_failures + 1 END",
                     node_id,
                     is_healthy ? "true" : "false",
                     latency_ms,
                     is_healthy ? "true" : "false");

    ret = SPI_execute(query.data, false, 0);
    if (ret != SPI_OK_INSERT)
        elog(WARNING, "Health worker: failed to update health for node %d", node_id);

    pfree(query.data);

    /*
     * Check if this node has exceeded the failure threshold.
     * If so, log a warning and mark the node as inactive in the catalog.
     */
    if (!is_healthy)
    {
        StringInfoData check_query;
        int threshold = orochi_health_check_failure_threshold;

        initStringInfo(&check_query);
        appendStringInfo(&check_query,
                         "SELECT consecutive_failures FROM orochi.orochi_node_health "
                         "WHERE node_id = %d",
                         node_id);

        ret = SPI_execute(check_query.data, true, 1);
        if (ret == SPI_OK_SELECT && SPI_processed > 0)
        {
            bool isnull;
            int failures;

            failures = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                                    SPI_tuptable->tupdesc,
                                                    1, &isnull));

            if (!isnull && failures >= threshold)
            {
                elog(WARNING,
                     "Health worker: node %d has been unreachable for %d consecutive checks "
                     "(threshold: %d), marking as inactive",
                     node_id, failures, threshold);

                /* Mark the node as inactive in the catalog */
                StringInfoData update_query;

                initStringInfo(&update_query);
                appendStringInfo(&update_query,
                                 "UPDATE orochi.orochi_nodes SET is_active = false "
                                 "WHERE node_id = %d",
                                 node_id);

                ret = SPI_execute(update_query.data, false, 0);
                if (ret != SPI_OK_UPDATE)
                    elog(WARNING,
                         "Health worker: failed to mark node %d as inactive",
                         node_id);

                pfree(update_query.data);
            }
        }

        pfree(check_query.data);
    }
}

/* ============================================================
 * Background Worker Entry Point
 * ============================================================ */

/*
 * orochi_health_worker_main
 *    Entry point for the health check background worker.
 *    Periodically checks all registered nodes and updates their
 *    health status in the catalog.
 */
void
orochi_health_worker_main(Datum main_arg)
{
    /* Set up signal handlers */
    pqsignal(SIGHUP, health_sighup_handler);
    pqsignal(SIGTERM, health_sigterm_handler);

    BackgroundWorkerUnblockSignals();

    /* Connect to the database for SPI access */
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    elog(LOG, "Orochi health check worker started (interval: %d seconds)",
         orochi_health_check_interval);

    /* Main event loop */
    while (!got_sigterm)
    {
        int rc;
        int interval_ms = orochi_health_check_interval * 1000;

        /* Wait for timeout or signal */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       interval_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Handle SIGHUP - reload config */
        if (got_sighup)
        {
            got_sighup = false;
            ProcessConfigFile(PGC_SIGHUP);
            elog(LOG, "Health check worker reloaded configuration");
        }

        if (got_sigterm)
            break;

        /* Perform health checks inside a transaction */
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        SPI_connect();
        PushActiveSnapshot(GetTransactionSnapshot());

        /* Query all registered nodes */
        {
            int ret;
            uint64 i;

            ret = SPI_execute(
                "SELECT node_id, hostname, port FROM orochi.orochi_nodes",
                true, 0);

            if (ret == SPI_OK_SELECT && SPI_processed > 0)
            {
                TupleDesc tupdesc = SPI_tuptable->tupdesc;

                for (i = 0; i < SPI_processed; i++)
                {
                    HeapTuple tuple = SPI_tuptable->vals[i];
                    bool isnull;
                    int node_id;
                    char *hostname;
                    int port;
                    StringInfoData connstr;
                    bool is_healthy;
                    int latency_ms = 0;

                    node_id = DatumGetInt32(
                        SPI_getbinval(tuple, tupdesc, 1, &isnull));
                    if (isnull)
                        continue;

                    hostname = SPI_getvalue(tuple, tupdesc, 2);
                    if (hostname == NULL)
                        continue;

                    port = DatumGetInt32(
                        SPI_getbinval(tuple, tupdesc, 3, &isnull));
                    if (isnull)
                        port = 5432;

                    /* Build connection string */
                    initStringInfo(&connstr);
                    appendStringInfo(&connstr,
                                     "host=%s port=%d dbname=postgres "
                                     "connect_timeout=5",
                                     hostname, port);

                    /* Check this node's health */
                    is_healthy = check_node_health(connstr.data, &latency_ms);

                    elog(DEBUG1,
                         "Health check: node %d (%s:%d) - %s (latency: %d ms)",
                         node_id, hostname, port,
                         is_healthy ? "healthy" : "unreachable",
                         latency_ms);

                    /* Update the health record */
                    update_node_health(node_id, is_healthy, latency_ms);

                    pfree(connstr.data);
                }
            }
        }

        PopActiveSnapshot();
        SPI_finish();
        CommitTransactionCommand();
        pgstat_report_activity(STATE_IDLE, NULL);
    }

    elog(LOG, "Orochi health check worker shutting down");
    proc_exit(0);
}

#endif /* RAFT_INTEGRATION */
