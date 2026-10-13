#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT3

#include "rtree_bulk_load.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void *rb_allocate(RBLoadState *state, sqlite3_uint64 size)
{
    void *memory;

    if (size > (sqlite3_uint64)(size_t)-1) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "allocation size is not representable");
        return NULL;
    }
    if (state->max_bytes != 0 &&
        (size > state->max_bytes || state->memory_used > state->max_bytes - size)) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "max_bytes exceeded while building RTree");
        return NULL;
    }
    memory = sqlite3_malloc64(size);
    if (memory == NULL) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "out of memory while building RTree");
        return NULL;
    }
    state->memory_used += size;
    return memory;
}

void rb_deallocate(RBLoadState *state, void *memory, sqlite3_uint64 size)
{
    if (memory == NULL) return;
    sqlite3_free(memory);
    state->memory_used = size > state->memory_used ? 0 : state->memory_used - size;
}

static void set_db_error(RBLoadState *state, const char *prefix)
{
    if (prefix != NULL) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s: %s", prefix, sqlite3_errmsg(state->database));
    } else {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", sqlite3_errmsg(state->database));
    }
}

static int execute_sql(RBLoadState *state, const char *sql)
{
    char *error = NULL;
    int result = sqlite3_exec(state->database, sql, NULL, NULL, &error);

    if (result != SQLITE_OK) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", error != NULL ? error : sqlite3_errmsg(state->database));
    }
    sqlite3_free(error);
    return result;
}

static int prepare_source(RBLoadState *state, const char *sql,
                          sqlite3_stmt **statement)
{
    sqlite3_stmt *trailing = NULL;
    const char *tail = NULL;
    int result = sqlite3_prepare_v2(state->database, sql, -1, statement, &tail);

    if (result != SQLITE_OK) {
        set_db_error(state, "could not prepare source query");
        return result;
    }
    if (*statement == NULL) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "source query is empty");
        return SQLITE_ERROR;
    }
    if (!sqlite3_stmt_readonly(*statement) ||
        sqlite3_column_count(*statement) != 5) {
        sqlite3_finalize(*statement);
        *statement = NULL;
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "source must be a read-only query returning five columns");
        return SQLITE_MISUSE;
    }
    result = sqlite3_prepare_v2(state->database, tail, -1, &trailing, NULL);
    if (result != SQLITE_OK || trailing != NULL) {
        sqlite3_finalize(trailing);
        sqlite3_finalize(*statement);
        *statement = NULL;
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "only one source statement is allowed");
        return SQLITE_MISUSE;
    }
    return SQLITE_OK;
}

static int grow_records(RBLoadState *state, RBRecord **records,
                        size_t *capacity, size_t needed)
{
    size_t new_capacity = *capacity == 0 ? 16 : *capacity;
    sqlite3_uint64 old_bytes = (sqlite3_uint64)(*capacity) * sizeof(**records);
    sqlite3_uint64 new_bytes;
    RBRecord *grown;

    while (new_capacity < needed) {
        if (new_capacity > (size_t)-1 / 2) {
            sqlite3_snprintf((int)sizeof(state->error), state->error,
                             "%s", "too many source rows");
            return SQLITE_TOOBIG;
        }
        new_capacity *= 2;
    }
    if (new_capacity > (size_t)-1 / sizeof(**records)) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "too many source rows");
        return SQLITE_TOOBIG;
    }
    new_bytes = (sqlite3_uint64)new_capacity * sizeof(**records);
    if (state->max_bytes != 0 &&
        (new_bytes > state->max_bytes ||
         state->memory_used - old_bytes > state->max_bytes - new_bytes)) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "max_bytes exceeded while reading source query");
        return SQLITE_NOMEM;
    }
    grown = (RBRecord *)sqlite3_realloc64(*records, new_bytes);
    if (grown == NULL) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "out of memory while reading source query");
        return SQLITE_NOMEM;
    }
    state->memory_used = state->memory_used - old_bytes + new_bytes;
    *records = grown;
    *capacity = new_capacity;
    return SQLITE_OK;
}

static int convert_coordinate(RBLoadState *state, double value,
                              int round_down, float *result)
{
    float converted;

    if (!isfinite(value) || value < -(double)FLT_MAX || value > (double)FLT_MAX) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "coordinates must be finite and fit in a 32-bit float");
        return SQLITE_RANGE;
    }
    converted = (float)value;
    if (round_down && (double)converted > value) {
        converted = nextafterf(converted, -FLT_MAX);
    } else if (!round_down && (double)converted < value) {
        converted = nextafterf(converted, FLT_MAX);
    }
    if (!isfinite((double)converted)) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "coordinate rounding exceeded float range");
        return SQLITE_RANGE;
    }
    *result = converted;
    return SQLITE_OK;
}

static int read_bound(sqlite3_stmt *statement, int column, double *value)
{
    const int type = sqlite3_column_type(statement, column);

    if (type != SQLITE_INTEGER && type != SQLITE_FLOAT) return SQLITE_MISMATCH;
    *value = sqlite3_column_double(statement, column);
    return SQLITE_OK;
}

static int read_records(RBLoadState *state, sqlite3_stmt *source,
                        RBRecord **records_out, size_t *count_out,
                        size_t *capacity_out)
{
    RBRecord *records = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result;

    while ((result = sqlite3_step(source)) == SQLITE_ROW) {
        RBRecord record;
        double min_x;
        double min_y;
        double max_x;
        double max_y;

        if (sqlite3_column_type(source, 0) != SQLITE_INTEGER ||
            read_bound(source, 1, &min_x) != SQLITE_OK ||
            read_bound(source, 2, &min_y) != SQLITE_OK ||
            read_bound(source, 3, &max_x) != SQLITE_OK ||
            read_bound(source, 4, &max_y) != SQLITE_OK) {
            sqlite3_snprintf((int)sizeof(state->error), state->error,
                             "%s", "each source row must contain an integer ID and four numeric bounds");
            result = SQLITE_MISMATCH;
            break;
        }
        if (!isfinite(min_x) || !isfinite(min_y) ||
            !isfinite(max_x) || !isfinite(max_y) ||
            min_x > max_x || min_y > max_y) {
            sqlite3_snprintf((int)sizeof(state->error), state->error,
                             "%s", "bounds must be finite with min <= max");
            result = SQLITE_CONSTRAINT;
            break;
        }
        if (count == (size_t)-1) {
            sqlite3_snprintf((int)sizeof(state->error), state->error,
                             "%s", "too many source rows");
            result = SQLITE_TOOBIG;
            break;
        }
        if (count == capacity) {
            result = grow_records(state, &records, &capacity, count + 1);
            if (result != SQLITE_OK) break;
        }
        record.id = sqlite3_column_int64(source, 0);
        record.ordinal = (sqlite3_uint64)count;
        result = convert_coordinate(state, min_x, 1, &record.bounds.min_x);
        if (result == SQLITE_OK) result = convert_coordinate(state, min_y, 1, &record.bounds.min_y);
        if (result == SQLITE_OK) result = convert_coordinate(state, max_x, 0, &record.bounds.max_x);
        if (result == SQLITE_OK) result = convert_coordinate(state, max_y, 0, &record.bounds.max_y);
        if (result != SQLITE_OK) break;
        records[count++] = record;
    }

    if (result == SQLITE_DONE) result = SQLITE_OK;
    else if (result != SQLITE_OK && state->error[0] == '\0') set_db_error(state, "source query failed");
    if (result != SQLITE_OK) {
        if (records != NULL) {
            rb_deallocate(state, records, (sqlite3_uint64)capacity * sizeof(*records));
        }
        return result;
    }

    *records_out = records;
    *count_out = count;
    *capacity_out = capacity;
    return SQLITE_OK;
}

static int compare_record_id(const void *left_ptr, const void *right_ptr)
{
    const RBRecord *left = (const RBRecord *)left_ptr;
    const RBRecord *right = (const RBRecord *)right_ptr;

    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    if (left->ordinal < right->ordinal) return -1;
    if (left->ordinal > right->ordinal) return 1;
    return 0;
}

static int compare_record_ordinal(const void *left_ptr, const void *right_ptr)
{
    const RBRecord *left = (const RBRecord *)left_ptr;
    const RBRecord *right = (const RBRecord *)right_ptr;

    if (left->ordinal < right->ordinal) return -1;
    if (left->ordinal > right->ordinal) return 1;
    return 0;
}

static int validate_unique_ids(RBLoadState *state, RBRecord *records,
                               size_t count)
{
    size_t i;

    if (count < 2) return SQLITE_OK;
    qsort(records, count, sizeof(*records), compare_record_id);
    for (i = 1; i < count; ++i) {
        if (records[i - 1].id == records[i].id) {
            sqlite3_snprintf((int)sizeof(state->error), state->error,
                             "%s", "source IDs must be unique");
            return SQLITE_CONSTRAINT;
        }
    }
    qsort(records, count, sizeof(*records), compare_record_ordinal);
    return SQLITE_OK;
}

static int get_node_capacity(RBLoadState *state, int build_str)
{
    sqlite3_stmt *statement = NULL;
    int page_size;
    int result;

    result = sqlite3_prepare_v2(state->database, "PRAGMA page_size", -1,
                                &statement, NULL);

    if (result != SQLITE_OK) {
        set_db_error(state, "could not read SQLite page size");
        return result;
    }
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        sqlite3_finalize(statement);
        set_db_error(state, "could not read SQLite page size");
        return SQLITE_ERROR;
    }
    page_size = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    state->page_size = page_size;
    if (sizeof(float) != 4 || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        FLT_MIN_EXP != -125 || FLT_MAX_EXP != 128) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "SQLite RTree shadow serialization requires IEEE binary32 floats");
        return SQLITE_FORMAT;
    }
    if (page_size < 512) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "unsupported SQLite page size");
        return SQLITE_FORMAT;
    }
    if (!build_str) return SQLITE_OK;
    state->node_capacity = (page_size - 68) / 24;
    if (state->node_capacity > RB_NODE_MAX_CAPACITY) {
        state->node_capacity = RB_NODE_MAX_CAPACITY;
    }
    if (state->node_capacity < 2) {
        sqlite3_snprintf((int)sizeof(state->error), state->error,
                         "%s", "SQLite page size yields an invalid RTree node capacity");
        return SQLITE_FORMAT;
    }
    return SQLITE_OK;
}

int rb_bulk_load(RBLoadState *state, const char *index_name,
                 const char *select_sql, const char *algorithm,
                 sqlite3_uint64 max_bytes, sqlite3_int64 *loaded_rows)
{
    sqlite3_stmt *source = NULL;
    RBRecord *records = NULL;
    size_t record_count = 0;
    size_t record_capacity = 0;
    RBNode *root = NULL;
    int savepoint_started = 0;
    int result;

    state->max_bytes = max_bytes;
    state->memory_used = 0;
    state->error[0] = '\0';

    result = get_node_capacity(state, sqlite3_stricmp(algorithm, "str") == 0);
    if (result != SQLITE_OK) return result;
    result = execute_sql(state, "SAVEPOINT rtree_bulk_load_internal");
    if (result != SQLITE_OK) return result;
    savepoint_started = 1;

    result = prepare_source(state, select_sql, &source);
    if (result == SQLITE_OK) {
        result = read_records(state, source, &records, &record_count,
                              &record_capacity);
    }
    if (source != NULL) {
        const int finalize_result = sqlite3_finalize(source);
        source = NULL;
        if (result == SQLITE_OK && finalize_result != SQLITE_OK) {
            result = finalize_result;
            set_db_error(state, "source query finalization failed");
        }
    }
    if (result == SQLITE_OK) result = validate_unique_ids(state, records, record_count);
    if (result == SQLITE_OK && sqlite3_stricmp(algorithm, "str") == 0) {
        result = rb_build_str(state, records, record_count, &root);
        if (result == SQLITE_OK) result = rb_write_tree(state, index_name, root);
    } else if (result == SQLITE_OK) {
        result = rb_write_rstar(state, index_name, records, record_count);
    }

    rb_free_tree(state, root);
    if (records != NULL) {
        rb_deallocate(state, records,
                      (sqlite3_uint64)record_capacity * sizeof(*records));
    }
    if (result == SQLITE_OK) {
        result = execute_sql(state, "RELEASE rtree_bulk_load_internal");
        if (result == SQLITE_OK) savepoint_started = 0;
    }
    if (result != SQLITE_OK && savepoint_started) {
        sqlite3_exec(state->database, "ROLLBACK TO rtree_bulk_load_internal",
                     NULL, NULL, NULL);
        sqlite3_exec(state->database, "RELEASE rtree_bulk_load_internal",
                     NULL, NULL, NULL);
    }
    if (result == SQLITE_OK) *loaded_rows = (sqlite3_int64)record_count;
    return result;
}