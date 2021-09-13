/* Some general headers for custom bgworker facility */
#include "postgres.h"
#include "fmgr.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "executor/spi.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "funcapi.h"
#include "catalog/pg_authid.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "commands/dbcommands.h"
#include "utils/resowner.h"

#include "pg_tap_counter.h"

/* Allow load of this module in shared libs */
PG_MODULE_MAGIC;

/* Entry point of library loading */
void _PG_init(void);
void _PG_fini(void);

/* Shared memory init */
static void pgtc_shmem_startup(void);

/* Signal handling */
static volatile sig_atomic_t got_sigterm = false;

/* One interval in buffer to count messages (s) */
static int interval_s = 10;

static int buffer_size_mb;

/* Worker name */
static char *worker_name = "pgtc worker";

static char *extension_name = "pg_tap_counter";

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
pgtc_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    if (MyProc)
        SetLatch(&MyProc->procLatch);
    errno = save_errno;
}

void pgtc_main(Datum) pg_attribute_noreturn();

void
pgtc_main(Datum main_arg)
{
    /* Register functions for SIGTERM management */
    pqsignal(SIGTERM, pgtc_sigterm);

    /* We're now ready to receive signals */
    BackgroundWorkerUnblockSignals();

    while (!got_sigterm)
    {
        int rc;
        /* Wait necessary amount of time */
        rc = WaitLatch(&MyProc->procLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, interval_s * 1e3, PG_WAIT_EXTENSION);

        ResetLatch(&MyProc->procLatch);
        /* Emergency bailout if postmaster has died */
        if (rc & WL_POSTMASTER_DEATH)
            proc_exit(1);
        /* Process signals */

        if (got_sigterm)
        {
            /* Simply exit */
            elog(DEBUG1, "pgtc: processed SIGTERM");
            proc_exit(0);
        }
        /* Main work happens here */
        pgtb_tick(extension_name);
    }

    /* No problems, so clean exit */
    proc_exit(0);
}


static void
define_custom_variables(void) {
    DefineCustomIntVariable("pg_tap_counter.buffer_size",
                            "Max amount of shared memory (in megabytes), that can be allocated",
                            "Default of 10, max of 5120",
                            &buffer_size_mb,
                            20,
                            1,
                            5120,
                            PGC_SUSET,
                            GUC_UNIT_MB | GUC_NO_RESET_ALL,
                            NULL,
                            NULL,
                            NULL);
}
/*
 * Entry point for worker loading
 */
void
_PG_init(void)
{
    BackgroundWorker worker;
    if (!process_shared_preload_libraries_in_progress) {
        return;
    }
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgtc_shmem_startup;
    define_custom_variables();
    elog(WARNING, "allocate %d ", buffer_size_mb * 1024 * 1024);
    RequestAddinShmemSpace(buffer_size_mb * 1024 * 1024);
    /* Worker parameter and registration */
    MemSet(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_PostmasterStart;
    snprintf(worker.bgw_name, BGW_MAXLEN, "%s", worker_name);
    sprintf(worker.bgw_library_name, "pg_tap_counter");
    sprintf(worker.bgw_function_name, "pgtc_main");
    /* Wait 10 seconds for restart after crash */
    worker.bgw_restart_time = 10;
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
}

void
_PG_fini(void)
{
    shmem_startup_hook = prev_shmem_startup_hook;
}

void add(void* value, void* anotherValue) {
    pgtcValue* pgtc_value;
    pgtcValue* pgtc_another_value;

    pgtc_value = (pgtcValue*) value;
    pgtc_another_value = (pgtcValue*) anotherValue;

    pgtc_value->count += pgtc_another_value->count;
}

void on_delete(void* key, void* value) {
    pgtcKey* pgtc_key;
    pgtcValue* pgtc_value;

    pgtc_key = (pgtcKey*) key;
    pgtc_value = (pgtcValue*) value;
    elog(LOG, "pgtc: %d of keys %d was deleted", pgtc_value->count, pgtc_key->foo);
}

static void
pgtc_shmem_startup(void) {
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    pgtb_init(extension_name, &add, &on_delete, interval_s, buffer_size_mb, sizeof(pgtcKey), sizeof(pgtcValue));
    //if (!IsUnderPostmaster) {
    // do something
    //}
    return;
}

PG_FUNCTION_INFO_V1(pgtc_tap);

Datum
pgtc_tap(PG_FUNCTION_ARGS) {
    pgtcKey key;
    pgtcValue value;
    memset(&key, 0, sizeof(pgtcKey));
    key.foo = 0;
    memset(&value, 0, sizeof(pgtcValue));
    value.count = 1;
    pgtb_put(extension_name, &key, &value);
    elog(LOG, "tap");
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgtc_reset);

Datum
pgtc_reset(PG_FUNCTION_ARGS) {
    pgtb_reset_stats(extension_name);
    elog(LOG, "reset");
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgtc_show);

Datum
pgtc_show(PG_FUNCTION_ARGS) {
    void* result_ptr;
    int length;
    TimestampTz ts_left;
    TimestampTz ts_right;
    char left_ts_s[50];
    char right_ts_s[50];


    result_ptr = (void*) palloc(sizeof(pgtcKey) + sizeof(pgtcValue));
    memset(result_ptr, 0, sizeof(pgtcKey) + sizeof(pgtcValue));
    pgtb_get_stats(extension_name, result_ptr, &length, &ts_left, &ts_right);
    strcpy(left_ts_s, timestamptz_to_str(ts_left));
    strcpy(right_ts_s, timestamptz_to_str(ts_right));
    elog(NOTICE, "pgsk: Show stats from '%s' to '%s'", left_ts_s, right_ts_s);
    elog(NOTICE, "pgtc: result is %d", ((pgtcValue*)((char*)result_ptr + sizeof(pgtcKey)))->count);
    pfree(result_ptr);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgtc_show_by_time);

Datum
pgtc_show_by_time(PG_FUNCTION_ARGS) {
    void* result_ptr;
    int length;
    TimestampTz timestamp_left;
    TimestampTz timestamp_right;
    char timestamp_left_s[50];
    char timestamp_right_s[50];

    timestamp_left = PG_GETARG_TIMESTAMP(0);
    timestamp_right = PG_GETARG_TIMESTAMP(1);
    result_ptr = (void*) palloc(sizeof(pgtcKey) + sizeof(pgtcValue));
    memset(result_ptr, 0, sizeof(pgtcKey) + sizeof(pgtcValue));
    pgtb_get_stats_time_interval(extension_name, &timestamp_left, &timestamp_right, result_ptr, &length);

    strcpy(timestamp_left_s, timestamptz_to_str(timestamp_left));
    strcpy(timestamp_right_s, timestamptz_to_str(timestamp_right));

    elog(NOTICE, "pgsk: Show stats from '%s' to '%s'", timestamp_left_s, timestamp_right_s);
    elog(NOTICE, "pgtc: result is %d", ((pgtcValue*)((char*)result_ptr + sizeof(pgtcKey)))->count);
    pfree(result_ptr);
    PG_RETURN_VOID();
}
