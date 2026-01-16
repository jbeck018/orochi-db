/*-------------------------------------------------------------------------
 *
 * postgres_mock.h
 *    Mock PostgreSQL types for standalone unit testing
 *
 * This header provides minimal type definitions to allow testing
 * Orochi DB modules without linking against PostgreSQL.
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_MOCK_H
#define POSTGRES_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================
 * Basic PostgreSQL Types
 * ============================================================ */

/* Object identifiers */
typedef unsigned int Oid;
#define InvalidOid      ((Oid) 0)

/* Datum - the main data type for passing values */
typedef uintptr_t Datum;

/* Size type */
typedef size_t Size;

/* Integer types matching PostgreSQL conventions */
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* Timestamps */
typedef int64 TimestampTz;

/* Interval (simplified) */
typedef struct Interval
{
    int64       time;           /* Microseconds */
    int32       day;            /* Days */
    int32       month;          /* Months */
} Interval;

/* ============================================================
 * PostgreSQL Memory and Varlena Types
 * ============================================================ */

/* Varlena header - variable-length data */
typedef struct varlena
{
    char        vl_len_[4];     /* Length word with flags */
    char        vl_dat[1];      /* Data content (flexible) */
} varlena;

/* bytea is just varlena */
typedef varlena bytea;

/* Text is also varlena */
typedef varlena text;

/* Varlena macros (simplified - assume 4-byte aligned) */
#define SET_VARSIZE(ptr, len)   (((int32 *)(ptr))[0] = (len))
#define VARSIZE(ptr)            (((int32 *)(ptr))[0])
#define VARDATA(ptr)            (((varlena *)(ptr))->vl_dat)
#define VARSIZE_ANY(ptr)        VARSIZE(ptr)
#define VARSIZE_ANY_EXHDR(ptr)  (VARSIZE(ptr) - offsetof(varlena, vl_dat))
#define VARDATA_ANY(ptr)        VARDATA(ptr)

/* Flexible array member */
#ifndef FLEXIBLE_ARRAY_MEMBER
#define FLEXIBLE_ARRAY_MEMBER   /* empty - C99 flexible array */
#endif

/* Max alignment */
#ifndef MAXALIGN
#define MAXALIGN(len)   (((len) + 7) & ~7)
#endif

/* ============================================================
 * PostgreSQL Function Call Interface (Mock)
 * ============================================================ */

/* Function call info - simplified mock */
typedef struct FunctionCallInfoBaseData
{
    bool        isnull;         /* Return value is null? */
    short       nargs;          /* Number of arguments */
    Datum       arg[16];        /* Arguments (max 16 for simplicity) */
    bool        argnull[16];    /* Null flags for arguments */
} FunctionCallInfoBaseData;

typedef FunctionCallInfoBaseData *FunctionCallInfo;

/* PG_FUNCTION_ARGS macro */
#define PG_FUNCTION_ARGS    FunctionCallInfo fcinfo

/* Argument access macros */
#define PG_GETARG_DATUM(n)      (fcinfo->arg[n])
#define PG_GETARG_INT32(n)      ((int32) fcinfo->arg[n])
#define PG_GETARG_INT64(n)      ((int64) fcinfo->arg[n])
#define PG_GETARG_FLOAT4(n)     (*((float *) &fcinfo->arg[n]))
#define PG_GETARG_FLOAT8(n)     (*((double *) &fcinfo->arg[n]))
#define PG_GETARG_BOOL(n)       ((bool) fcinfo->arg[n])
#define PG_GETARG_POINTER(n)    ((void *) fcinfo->arg[n])
#define PG_GETARG_OID(n)        ((Oid) fcinfo->arg[n])
#define PG_GETARG_BYTEA_PP(n)   ((bytea *) fcinfo->arg[n])
#define PG_ARGISNULL(n)         (fcinfo->argnull[n])

/* Return macros */
#define PG_RETURN_DATUM(x)      return (Datum)(x)
#define PG_RETURN_INT32(x)      return (Datum)(x)
#define PG_RETURN_INT64(x)      return (Datum)(x)
#define PG_RETURN_FLOAT8(x)     do { double _d = (x); return *((Datum *)&_d); } while(0)
#define PG_RETURN_BOOL(x)       return (Datum)(x)
#define PG_RETURN_POINTER(x)    return (Datum)(x)
#define PG_RETURN_BYTEA_P(x)    return (Datum)(x)
#define PG_RETURN_NULL()        do { fcinfo->isnull = true; return (Datum)0; } while(0)

/* Detoast macro (no-op in mock) */
#define PG_DETOAST_DATUM(x)     ((void *)(x))

/* ============================================================
 * PostgreSQL Memory Context (Mock)
 * ============================================================ */

/* Simple palloc/pfree using malloc/free */
#define palloc(size)            malloc(size)
#define palloc0(size)           calloc(1, size)
#define pfree(ptr)              free(ptr)
#define repalloc(ptr, size)     realloc(ptr, size)

/* Allocate in current memory context (just malloc for mock) */
#define MemoryContextAlloc(ctx, size)   malloc(size)
#define MemoryContextAllocZero(ctx, size)   calloc(1, size)

/* Memory context type (placeholder) */
typedef void *MemoryContext;

/* ============================================================
 * PostgreSQL List Types (Simplified)
 * ============================================================ */

typedef struct ListCell
{
    void       *ptr_value;
    struct ListCell *next;
} ListCell;

typedef struct List
{
    int         length;
    ListCell   *head;
    ListCell   *tail;
} List;

#define NIL     ((List *) NULL)

/* List iteration macros */
#define foreach(cell, lst) \
    for ((cell) = (lst) ? (lst)->head : NULL; (cell) != NULL; (cell) = (cell)->next)

#define lfirst(lc)              ((lc)->ptr_value)
#define lfirst_int(lc)          ((int)(intptr_t)(lc)->ptr_value)
#define lfirst_oid(lc)          ((Oid)(intptr_t)(lc)->ptr_value)

/* List construction */
static inline List *lappend(List *list, void *datum)
{
    ListCell *cell = (ListCell *)malloc(sizeof(ListCell));
    cell->ptr_value = datum;
    cell->next = NULL;

    if (list == NULL)
    {
        list = (List *)malloc(sizeof(List));
        list->length = 0;
        list->head = NULL;
        list->tail = NULL;
    }

    if (list->tail)
        list->tail->next = cell;
    else
        list->head = cell;

    list->tail = cell;
    list->length++;
    return list;
}

static inline int list_length(List *l)
{
    return l ? l->length : 0;
}

/* ============================================================
 * PostgreSQL Type OIDs (Common Ones)
 * ============================================================ */

#define BOOLOID         16
#define INT2OID         21
#define INT4OID         23
#define INT8OID         20
#define FLOAT4OID       700
#define FLOAT8OID       701
#define TEXTOID         25
#define BYTEAOID        17
#define TIMESTAMPTZOID  1184

/* ============================================================
 * PostgreSQL Error Reporting (Mock)
 * ============================================================ */

/* Error levels */
#define DEBUG5      10
#define DEBUG4      11
#define DEBUG3      12
#define DEBUG2      13
#define DEBUG1      14
#define LOG         15
#define INFO        17
#define NOTICE      18
#define WARNING     19
#define ERROR       20
#define FATAL       21
#define PANIC       22

/* Simplified elog/ereport - just print and exit on errors */
#include <stdio.h>

#define elog(level, fmt, ...) \
    do { \
        fprintf(stderr, "[%s] " fmt "\n", \
            (level) >= ERROR ? "ERROR" : "LOG", ##__VA_ARGS__); \
        if ((level) >= ERROR) exit(1); \
    } while(0)

#define ereport(level, rest) \
    do { \
        if ((level) >= ERROR) { \
            fprintf(stderr, "ERROR\n"); \
            exit(1); \
        } \
    } while(0)

/* Error detail macros (no-op in mock) */
#define errcode(code)           0
#define errmsg(fmt, ...)        0
#define errdetail(fmt, ...)     0
#define errhint(fmt, ...)       0

/* ============================================================
 * Miscellaneous PostgreSQL Macros
 * ============================================================ */

/* Min/Max macros */
#ifndef Min
#define Min(a, b)   ((a) < (b) ? (a) : (b))
#endif
#ifndef Max
#define Max(a, b)   ((a) > (b) ? (a) : (b))
#endif

/* Assert macro */
#ifndef Assert
#ifdef USE_ASSERT_CHECKING
#define Assert(cond)    do { if (!(cond)) abort(); } while(0)
#else
#define Assert(cond)    ((void)0)
#endif
#endif

/* Static assert */
#ifndef StaticAssertStmt
#define StaticAssertStmt(cond, msg) _Static_assert(cond, msg)
#endif

/* Likely/Unlikely hints */
#ifndef likely
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#endif

/* Integer to pointer casts */
#define Int32GetDatum(x)    ((Datum)(int32)(x))
#define DatumGetInt32(x)    ((int32)(x))
#define Int64GetDatum(x)    ((Datum)(int64)(x))
#define DatumGetInt64(x)    ((int64)(x))
#define Float8GetDatum(x)   (*((Datum *)&(x)))
#define DatumGetFloat8(x)   (*((double *)&(x)))
#define PointerGetDatum(x)  ((Datum)(x))
#define DatumGetPointer(x)  ((void *)(x))
#define BoolGetDatum(x)     ((Datum)(x))
#define DatumGetBool(x)     ((bool)(x))

/* Offsetof if not defined */
#ifndef offsetof
#define offsetof(type, field)   ((size_t)&((type *)0)->field)
#endif

/* PostgreSQL module magic (no-op in mock) */
#define PG_MODULE_MAGIC
#define PG_FUNCTION_INFO_V1(func)

#endif /* POSTGRES_MOCK_H */
