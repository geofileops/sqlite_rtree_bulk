#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT3

#define SQLITE_RTREE_BL_SYMBOL(name) rtreebulkload_vendor_##name
#include "../vendor/sqlite_rtree_bulk_load/sqlite_rtree_bulk_load.c"