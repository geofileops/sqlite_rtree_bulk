#ifndef RTREE_BULK_LOAD_H
#define RTREE_BULK_LOAD_H

#include <sqlite3.h>

#include <stddef.h>
#include <stdint.h>

#define RB_NODE_MAX_CAPACITY 51

typedef struct RBRect {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} RBRect;

typedef struct RBRecord {
    sqlite3_int64 id;
    RBRect bounds;
    sqlite3_uint64 ordinal;
} RBRecord;

typedef struct RBNode RBNode;

typedef struct RBEntry {
    RBRect bounds;
    sqlite3_int64 id;
    sqlite3_uint64 ordinal;
    RBNode *child;
} RBEntry;

struct RBNode {
    int level;
    int count;
    sqlite3_int64 stable_id;
    sqlite3_uint64 stable_ordinal;
    sqlite3_int64 node_number;
    sqlite3_int64 parent_number;
    RBRect bounds;
    RBEntry entries[RB_NODE_MAX_CAPACITY];
};

typedef struct RBLoadState {
    sqlite3 *database;
    sqlite3_uint64 max_bytes;
    sqlite3_uint64 memory_used;
    int page_size;
    int node_capacity;
    char error[256];
} RBLoadState;

void *rb_allocate(RBLoadState *state, sqlite3_uint64 size);
void rb_deallocate(RBLoadState *state, void *memory, sqlite3_uint64 size);
int rb_build_str(RBLoadState *state, const RBRecord *records, size_t count,
                 RBNode **root);
void rb_free_tree(RBLoadState *state, RBNode *node);
int rb_write_tree(RBLoadState *state, const char *index_name, RBNode *root);
int rb_write_rstar(RBLoadState *state, const char *index_name,
                   const RBRecord *records, size_t count);
int rb_bulk_load(RBLoadState *state, const char *index_name,
                 const char *select_sql, const char *algorithm,
                 sqlite3_uint64 max_bytes, sqlite3_int64 *loaded_rows);

#endif