#include "postgres.h"

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/jsonb.h"
#include "executor/execdesc.h"
#include "nodes/execnodes.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

#include "pg_time_buffer.h"

void pgtc_main(Datum)pg_attribute_noreturn();

typedef struct pgtcKey {
    int foo;
} pgtcKey;

typedef struct pgtcValue {
    int count;
} pgtcValue;

void add(void*, void*);
void on_delete(void*, void*);
