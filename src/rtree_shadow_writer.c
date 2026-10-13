#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT3

#include "rtree_bulk_load.h"
#include "rstar_vendor.h"

#include <stdint.h>
#include <string.h>

#define RB_CELL_BYTES (sizeof(sqlite3_int64) + 4 * sizeof(float))
#define RB_BLOB_BYTES (4 + RB_NODE_MAX_CAPACITY * RB_CELL_BYTES)

typedef struct RBWriteContext {
    RBLoadState *state;
    sqlite3_stmt *node_insert;
    sqlite3_stmt *parent_insert;
    sqlite3_stmt *rowid_insert;
    uint8_t blob[RB_BLOB_BYTES];
} RBWriteContext;

static void write_be16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void write_be64(uint8_t *destination, uint64_t value)
{
    int i;
    for (i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (56 - 8 * i));
    }
}

static void write_be_float(uint8_t *destination, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    destination[0] = (uint8_t)(bits >> 24);
    destination[1] = (uint8_t)(bits >> 16);
    destination[2] = (uint8_t)(bits >> 8);
    destination[3] = (uint8_t)bits;
}

static int execute(RBLoadState *state, const char *sql)
{
    char *error = NULL;
    int result = sqlite3_exec(state->database, sql, NULL, NULL, &error);

    if (result != SQLITE_OK) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         error != NULL ? error : sqlite3_errmsg(state->database));
    }
    sqlite3_free(error);
    return result;
}

static int create_index(RBLoadState *state, const char *index_name)
{
    char *sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%w\"", index_name);
    int result;

    if (sql == NULL) return SQLITE_NOMEM;
    result = execute(state, sql);
    sqlite3_free(sql);
    if (result != SQLITE_OK) return result;

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE \"%w\" USING rtree(id, minx, maxx, miny, maxy)",
        index_name);
    if (sql == NULL) return SQLITE_NOMEM;
    result = execute(state, sql);
    sqlite3_free(sql);
    return result;
}

static int prepare_insert(RBLoadState *state, const char *index_name,
                          const char *suffix, sqlite3_stmt **statement)
{
    char *sql = sqlite3_mprintf("INSERT INTO \"%w_%w\" VALUES (?, ?)",
                                index_name, suffix);
    int result;

    if (sql == NULL) return SQLITE_NOMEM;
    result = sqlite3_prepare_v2(state->database, sql, -1, statement, NULL);
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         sqlite3_errmsg(state->database));
    }
    return result;
}

static int assign_node_numbers(RBNode *node, sqlite3_int64 node_number,
                               sqlite3_int64 parent_number,
                               sqlite3_int64 *next_node_number)
{
    int i;

    node->node_number = node_number;
    node->parent_number = parent_number;
    if (node->level == 0) return SQLITE_OK;
    for (i = 0; i < node->count; ++i) {
        int result;
        RBNode *child = node->entries[i].child;

        if (child == NULL) return SQLITE_CORRUPT;
        result = assign_node_numbers(child, ++(*next_node_number), node_number,
                                     next_node_number);
        if (result != SQLITE_OK) return result;
    }
    return SQLITE_OK;
}

static int step_insert(RBLoadState *state, sqlite3_stmt *statement)
{
    int result = sqlite3_step(statement);

    if (result != SQLITE_DONE) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         sqlite3_errmsg(state->database));
        return result;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return SQLITE_OK;
}

static int write_node(RBWriteContext *context, const RBNode *node,
                      const RBNode *root)
{
    RBLoadState *state = context->state;
    const int blob_size = 4 + state->node_capacity * (int)RB_CELL_BYTES;
    int i;
    int result;
    size_t offset = 4;

    memset(context->blob, 0, sizeof(context->blob));
    write_be16(context->blob, (uint16_t)(node == root ? root->level : 0));
    write_be16(context->blob + 2, (uint16_t)node->count);
    for (i = 0; i < node->count; ++i) {
        const RBEntry *entry = &node->entries[i];
        const sqlite3_int64 id = node->level > 0
                                     ? entry->child->node_number : entry->id;

        write_be64(context->blob + offset, (uint64_t)id);
        offset += sizeof(sqlite3_int64);
        write_be_float(context->blob + offset, entry->bounds.min_x);
        offset += sizeof(float);
        write_be_float(context->blob + offset, entry->bounds.max_x);
        offset += sizeof(float);
        write_be_float(context->blob + offset, entry->bounds.min_y);
        offset += sizeof(float);
        write_be_float(context->blob + offset, entry->bounds.max_y);
        offset += sizeof(float);
    }

    sqlite3_bind_int64(context->node_insert, 1, node->node_number);
    sqlite3_bind_blob(context->node_insert, 2, context->blob, blob_size,
                      SQLITE_TRANSIENT);
    result = step_insert(state, context->node_insert);
    if (result != SQLITE_OK) return result;

    if (node->parent_number != 0) {
        sqlite3_bind_int64(context->parent_insert, 1, node->node_number);
        sqlite3_bind_int64(context->parent_insert, 2, node->parent_number);
        result = step_insert(state, context->parent_insert);
        if (result != SQLITE_OK) return result;
    }

    if (node->level == 0) {
        for (i = 0; i < node->count; ++i) {
            sqlite3_bind_int64(context->rowid_insert, 1, node->entries[i].id);
            sqlite3_bind_int64(context->rowid_insert, 2, node->node_number);
            result = step_insert(state, context->rowid_insert);
            if (result != SQLITE_OK) return result;
        }
    } else {
        for (i = 0; i < node->count; ++i) {
            result = write_node(context, node->entries[i].child, root);
            if (result != SQLITE_OK) return result;
        }
    }
    return SQLITE_OK;
}

static void finalize_writer(RBWriteContext *context)
{
    sqlite3_finalize(context->node_insert);
    sqlite3_finalize(context->parent_insert);
    sqlite3_finalize(context->rowid_insert);
}

int rb_write_tree(RBLoadState *state, const char *index_name, RBNode *root)
{
    RBWriteContext context;
    sqlite3_int64 next_node_number = 1;
    char *sql;
    int result;

    memset(&context, 0, sizeof(context));
    context.state = state;

    result = create_index(state, index_name);
    if (result != SQLITE_OK || root == NULL) return result;

    result = assign_node_numbers(root, 1, 0, &next_node_number);
    if (result != SQLITE_OK) return result;

    sql = sqlite3_mprintf("DELETE FROM \"%w_node\"", index_name);
    if (sql == NULL) return SQLITE_NOMEM;
    result = execute(state, sql);
    sqlite3_free(sql);
    if (result != SQLITE_OK) return result;

    result = prepare_insert(state, index_name, "node", &context.node_insert);
    if (result == SQLITE_OK) {
        result = prepare_insert(state, index_name, "parent", &context.parent_insert);
    }
    if (result == SQLITE_OK) {
        result = prepare_insert(state, index_name, "rowid", &context.rowid_insert);
    }
    if (result == SQLITE_OK) result = write_node(&context, root, root);
    finalize_writer(&context);
    return result;
}

int rb_write_rstar(RBLoadState *state, const char *index_name,
                   const RBRecord *records, size_t count)
{
    sqlite_rtree_bl *tree;
    char *error = NULL;
    char *sql;
    sqlite3_stmt *insert = NULL;
    size_t i;
    size_t packed_count = 0;
    int fallback = 0;
    int result;

    tree = rtreebulkload_vendor_sqlite_rtree_bl_new(state->page_size);
    if (tree == NULL) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         "unable to allocate the RStar builder");
        return SQLITE_NOMEM;
    }
    for (i = 0; i < count; ++i) {
        if (!rtreebulkload_vendor_sqlite_rtree_bl_insert(
                tree, records[i].id, records[i].bounds.min_x,
                records[i].bounds.min_y, records[i].bounds.max_x,
                records[i].bounds.max_y)) {
            sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                             "RStar insertion failed");
            rtreebulkload_vendor_sqlite_rtree_bl_free(tree);
            return SQLITE_NOMEM;
        }
        packed_count = i + 1;
        if (state->max_bytes != 0 &&
            (sqlite3_uint64)rtreebulkload_vendor_sqlite_rtree_bl_ram_usage(tree) >
                state->max_bytes - state->memory_used) {
            fallback = 1;
            break;
        }
    }

    sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%w\"", index_name);
    if (sql == NULL) {
        rtreebulkload_vendor_sqlite_rtree_bl_free(tree);
        return SQLITE_NOMEM;
    }
    result = execute(state, sql);
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        rtreebulkload_vendor_sqlite_rtree_bl_free(tree);
        return result;
    }

    result = rtreebulkload_vendor_sqlite_rtree_bl_serialize(
        tree, state->database, index_name, "id", "minx", "miny",
        "maxx", "maxy", &error) ? SQLITE_OK : SQLITE_ERROR;
    if (result != SQLITE_OK) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         error != NULL ? error : "RStar serialization failed");
    }
    sqlite3_free(error);
    rtreebulkload_vendor_sqlite_rtree_bl_free(tree);
    if (result != SQLITE_OK || !fallback || packed_count == count) return result;

    sql = sqlite3_mprintf("INSERT INTO \"%w\" VALUES (?, ?, ?, ?, ?)",
                          index_name);
    if (sql == NULL) return SQLITE_NOMEM;
    result = sqlite3_prepare_v2(state->database, sql, -1, &insert, NULL);
    sqlite3_free(sql);
    if (result != SQLITE_OK) {
        sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                         sqlite3_errmsg(state->database));
        return result;
    }

    for (i = packed_count; i < count && result == SQLITE_OK; ++i) {
        sqlite3_bind_int64(insert, 1, records[i].id);
        sqlite3_bind_double(insert, 2, records[i].bounds.min_x);
        sqlite3_bind_double(insert, 3, records[i].bounds.max_x);
        sqlite3_bind_double(insert, 4, records[i].bounds.min_y);
        sqlite3_bind_double(insert, 5, records[i].bounds.max_y);
        result = sqlite3_step(insert);
        if (result == SQLITE_DONE) {
            result = SQLITE_OK;
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        } else {
            sqlite3_snprintf((int)sizeof(state->error), state->error, "%s",
                             sqlite3_errmsg(state->database));
        }
    }
    sqlite3_finalize(insert);
    return result;
}