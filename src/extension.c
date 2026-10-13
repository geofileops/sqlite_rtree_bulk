#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "rtree_bulk_load.h"

#include <string.h>

#if defined(_WIN32)
#define RB_EXTENSION_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RB_EXTENSION_EXPORT __attribute__((visibility("default")))
#else
#define RB_EXTENSION_EXPORT
#endif

#define RB_DEFAULT_MAX_BYTES ((sqlite3_uint64)256 * 1024 * 1024)

static int read_text_argument(sqlite3_context *context, sqlite3_value *value,
                              const char **text_out, const char *label)
{
    const unsigned char *text;
    int bytes;

    if (sqlite3_value_type(value) != SQLITE_TEXT) {
        sqlite3_result_error(context, label, -1);
        return SQLITE_MISMATCH;
    }
    text = sqlite3_value_text(value);
    bytes = sqlite3_value_bytes(value);
    if (text == NULL || memchr(text, '\0', (size_t)bytes) != NULL) {
        sqlite3_result_error(context, label, -1);
        return SQLITE_MISMATCH;
    }
    *text_out = (const char *)text;
    return SQLITE_OK;
}

static int parse_options(sqlite3_context *context, int argc,
                         sqlite3_value **argv, int algorithm_index,
                         int limit_index, const char **algorithm_out,
                         sqlite3_uint64 *limit_out)
{
    const char *algorithm = "str";

    *limit_out = RB_DEFAULT_MAX_BYTES;
    if (argc > algorithm_index &&
        sqlite3_value_type(argv[algorithm_index]) != SQLITE_NULL) {
        if (read_text_argument(context, argv[algorithm_index], &algorithm,
                               "algorithm must be 'str' or 'rstar'") != SQLITE_OK) {
            return SQLITE_MISMATCH;
        }
    }
    if (sqlite3_stricmp(algorithm, "str") != 0 &&
        sqlite3_stricmp(algorithm, "rstar") != 0) {
        sqlite3_result_error(context, "algorithm must be 'str' or 'rstar'", -1);
        return SQLITE_MISMATCH;
    }

    if (argc > limit_index && sqlite3_value_type(argv[limit_index]) != SQLITE_NULL) {
        sqlite3_int64 requested;
        if (sqlite3_value_type(argv[limit_index]) != SQLITE_INTEGER) {
            sqlite3_result_error(context, "max_bytes must be a non-negative integer", -1);
            return SQLITE_MISMATCH;
        }
        requested = sqlite3_value_int64(argv[limit_index]);
        if (requested < 0) {
            sqlite3_result_error(context, "max_bytes must be a non-negative integer", -1);
            return SQLITE_MISMATCH;
        }
        *limit_out = (sqlite3_uint64)requested;
    }
    *algorithm_out = algorithm;
    return SQLITE_OK;
}

static void bulk_load_function(sqlite3_context *context, int argc,
                               sqlite3_value **argv, int spatialite_mode)
{
    sqlite3 *database = sqlite3_context_db_handle(context);
    const char *index_name = NULL;
    const char *select_sql = NULL;
    const char *algorithm = "str";
    sqlite3_uint64 max_bytes;
    sqlite3_int64 loaded_rows = 0;
    char *generated_sql = NULL;
    RBLoadState state;
    const int base_arguments = spatialite_mode ? 3 : 2;
    int result;

    if (argc < base_arguments || argc > base_arguments + 2) {
        sqlite3_result_error(context, spatialite_mode
            ? "expected index_name, source_table, geometry_column, optional algorithm and max_bytes"
            : "expected index_name, select_sql, optional algorithm and max_bytes", -1);
        return;
    }
    if (read_text_argument(context, argv[0], &index_name,
                           "index_name must be non-empty text") != SQLITE_OK) {
        return;
    }
    if (index_name[0] == '\0') {
        sqlite3_result_error(context, "index_name must be non-empty text", -1);
        return;
    }

    if (spatialite_mode) {
        const char *table_name = NULL;
        const char *geometry_column = NULL;
        if (read_text_argument(context, argv[1], &table_name,
                               "source_table must be text") != SQLITE_OK ||
            read_text_argument(context, argv[2], &geometry_column,
                               "geometry_column must be text") != SQLITE_OK) {
            return;
        }
        generated_sql = sqlite3_mprintf(
            "SELECT ROWID, MbrMinX(\"%w\"), MbrMinY(\"%w\"), "
            "MbrMaxX(\"%w\"), MbrMaxY(\"%w\") FROM \"%w\" "
            "WHERE MbrMinX(\"%w\") IS NOT NULL",
            geometry_column, geometry_column, geometry_column,
            geometry_column, table_name, geometry_column);
        if (generated_sql == NULL) {
            sqlite3_result_error_nomem(context);
            return;
        }
        select_sql = generated_sql;
    } else if (read_text_argument(context, argv[1], &select_sql,
                                  "select_sql must be text") != SQLITE_OK) {
        return;
    }

    result = parse_options(context, argc, argv, base_arguments,
                           base_arguments + 1, &algorithm, &max_bytes);
    if (result != SQLITE_OK) {
        sqlite3_free(generated_sql);
        return;
    }

    memset(&state, 0, sizeof(state));
    state.database = database;
    state.max_bytes = max_bytes;
    result = rb_bulk_load(&state, index_name, select_sql, algorithm,
                          max_bytes, &loaded_rows);
    sqlite3_free(generated_sql);
    if (result != SQLITE_OK) {
        sqlite3_result_error(context,
            state.error[0] != '\0' ? state.error : sqlite3_errmsg(database), -1);
        return;
    }
    sqlite3_result_int64(context, loaded_rows);
}

static void bulk_load_sql(sqlite3_context *context, int argc,
                          sqlite3_value **argv)
{
    bulk_load_function(context, argc, argv, 0);
}

static void bulk_load_spatialite_sql(sqlite3_context *context, int argc,
                                     sqlite3_value **argv)
{
    bulk_load_function(context, argc, argv, 1);
}

RB_EXTENSION_EXPORT int sqlite3_rtreebulkload_init(
    sqlite3 *database, char **error_message,
    const sqlite3_api_routines *api)
{
    int result;

    (void)error_message;
    SQLITE_EXTENSION_INIT2(api);
    result = sqlite3_create_function_v2(
        database, "rtree_bulk_load", -1,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, bulk_load_sql,
        NULL, NULL, NULL);
    if (result != SQLITE_OK) return result;
    return sqlite3_create_function_v2(
        database, "rtree_bulk_load_spatialite", -1,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, bulk_load_spatialite_sql,
        NULL, NULL, NULL);
}