#include "rtree_bulk_load.h"

#include <string.h>

typedef struct RBPackItem {
    RBRect bounds;
    sqlite3_int64 id;
    sqlite3_uint64 ordinal;
    RBNode *child;
} RBPackItem;

static double center_x(const RBRect *bounds)
{
    return ((double)bounds->min_x + (double)bounds->max_x) * 0.5;
}

static double center_y(const RBRect *bounds)
{
    return ((double)bounds->min_y + (double)bounds->max_y) * 0.5;
}

static int compare_x(const void *left_ptr, const void *right_ptr)
{
    const RBPackItem *left = (const RBPackItem *)left_ptr;
    const RBPackItem *right = (const RBPackItem *)right_ptr;
    const double left_x = center_x(&left->bounds);
    const double right_x = center_x(&right->bounds);
    const double left_y = center_y(&left->bounds);
    const double right_y = center_y(&right->bounds);

    if (left_x < right_x) return -1;
    if (left_x > right_x) return 1;
    if (left_y < right_y) return -1;
    if (left_y > right_y) return 1;
    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    if (left->ordinal < right->ordinal) return -1;
    if (left->ordinal > right->ordinal) return 1;
    return 0;
}

static int compare_y(const void *left_ptr, const void *right_ptr)
{
    const RBPackItem *left = (const RBPackItem *)left_ptr;
    const RBPackItem *right = (const RBPackItem *)right_ptr;
    const double left_y = center_y(&left->bounds);
    const double right_y = center_y(&right->bounds);
    const double left_x = center_x(&left->bounds);
    const double right_x = center_x(&right->bounds);

    if (left_y < right_y) return -1;
    if (left_y > right_y) return 1;
    if (left_x < right_x) return -1;
    if (left_x > right_x) return 1;
    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    if (left->ordinal < right->ordinal) return -1;
    if (left->ordinal > right->ordinal) return 1;
    return 0;
}

static void expand_rect(RBRect *target, const RBRect *other)
{
    if (other->min_x < target->min_x) target->min_x = other->min_x;
    if (other->min_y < target->min_y) target->min_y = other->min_y;
    if (other->max_x > target->max_x) target->max_x = other->max_x;
    if (other->max_y > target->max_y) target->max_y = other->max_y;
}

static size_t ceil_div(size_t value, size_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

static size_t slice_count_for(size_t count, size_t capacity)
{
    size_t leaf_count = ceil_div(count, capacity);
    size_t slices = 1;

    while (slices * slices < leaf_count) {
        ++slices;
    }
    return slices;
}

static size_t grouped_node_count(size_t count, size_t capacity,
                                 size_t slices)
{
    const size_t slice_size = ceil_div(count, slices);
    size_t node_count = 0;
    size_t start;

    for (start = 0; start < count; start += slice_size) {
        const size_t remaining = count - start;
        const size_t in_slice = remaining < slice_size ? remaining : slice_size;
        node_count += ceil_div(in_slice, capacity);
    }
    return node_count;
}

static int pack_level(RBLoadState *state, RBPackItem *items, size_t count,
                      int level, RBNode ***nodes_out, size_t *node_count_out)
{
    const size_t capacity = (size_t)state->node_capacity;
    const size_t slices = slice_count_for(count, capacity);
    const size_t slice_size = ceil_div(count, slices);
    const size_t node_count = grouped_node_count(count, capacity, slices);
    RBNode **nodes;
    size_t nodes_created = 0;
    size_t start;

    nodes = (RBNode **)rb_allocate(state, node_count * sizeof(*nodes));
    if (nodes == NULL) return SQLITE_NOMEM;
    qsort(items, count, sizeof(*items), compare_x);

    for (start = 0; start < count; start += slice_size) {
        const size_t remaining = count - start;
        const size_t in_slice = remaining < slice_size ? remaining : slice_size;
        size_t group_start;

        qsort(items + start, in_slice, sizeof(*items), compare_y);
        for (group_start = start; group_start < start + in_slice;
             group_start += capacity) {
            const size_t group_remaining = start + in_slice - group_start;
            const size_t in_group = group_remaining < capacity
                                        ? group_remaining : capacity;
            RBNode *node = (RBNode *)rb_allocate(state, sizeof(*node));
            size_t i;

            if (node == NULL) {
                size_t j;
                for (j = 0; j < nodes_created; ++j) {
                    rb_deallocate(state, nodes[j], sizeof(*nodes[j]));
                }
                rb_deallocate(state, nodes, node_count * sizeof(*nodes));
                return SQLITE_NOMEM;
            }
            memset(node, 0, sizeof(*node));
            node->level = level;
            node->count = (int)in_group;
            node->stable_id = items[group_start].id;
            node->stable_ordinal = items[group_start].ordinal;
            node->bounds = items[group_start].bounds;

            for (i = 0; i < in_group; ++i) {
                RBPackItem *item = &items[group_start + i];
                RBEntry *entry = &node->entries[i];

                entry->bounds = item->bounds;
                entry->id = item->id;
                entry->ordinal = item->ordinal;
                entry->child = item->child;
                if (i > 0) expand_rect(&node->bounds, &item->bounds);
                if (item->id < node->stable_id) node->stable_id = item->id;
                if (item->ordinal < node->stable_ordinal) {
                    node->stable_ordinal = item->ordinal;
                }
            }
            nodes[nodes_created++] = node;
        }
    }

    *nodes_out = nodes;
    *node_count_out = node_count;
    return SQLITE_OK;
}

int rb_build_str(RBLoadState *state, const RBRecord *records, size_t count,
                 RBNode **root)
{
    RBPackItem *items;
    RBNode **nodes = NULL;
    size_t node_count = 0;
    size_t i;
    int level = 0;
    int result;

    *root = NULL;
    if (count == 0) return SQLITE_OK;

    items = (RBPackItem *)rb_allocate(state, count * sizeof(*items));
    if (items == NULL) return SQLITE_NOMEM;
    for (i = 0; i < count; ++i) {
        items[i].bounds = records[i].bounds;
        items[i].id = records[i].id;
        items[i].ordinal = records[i].ordinal;
        items[i].child = NULL;
    }

    result = pack_level(state, items, count, level, &nodes, &node_count);
    rb_deallocate(state, items, count * sizeof(*items));
    if (result != SQLITE_OK) return result;

    while (node_count > 1) {
        RBPackItem *parents = (RBPackItem *)rb_allocate(
            state, node_count * sizeof(*parents));
        RBNode **next_nodes = NULL;
        size_t next_count = 0;

        if (parents == NULL) {
            for (i = 0; i < node_count; ++i) rb_free_tree(state, nodes[i]);
            rb_deallocate(state, nodes, node_count * sizeof(*nodes));
            return SQLITE_NOMEM;
        }
        for (i = 0; i < node_count; ++i) {
            parents[i].bounds = nodes[i]->bounds;
            parents[i].id = nodes[i]->stable_id;
            parents[i].ordinal = nodes[i]->stable_ordinal;
            parents[i].child = nodes[i];
        }
        result = pack_level(state, parents, node_count, ++level,
                            &next_nodes, &next_count);
        rb_deallocate(state, parents, node_count * sizeof(*parents));
        if (result != SQLITE_OK) {
            for (i = 0; i < node_count; ++i) rb_free_tree(state, nodes[i]);
            rb_deallocate(state, nodes, node_count * sizeof(*nodes));
            return result;
        }
        rb_deallocate(state, nodes, node_count * sizeof(*nodes));
        nodes = next_nodes;
        node_count = next_count;
    }

    *root = nodes[0];
    rb_deallocate(state, nodes, sizeof(*nodes));
    return SQLITE_OK;
}

void rb_free_tree(RBLoadState *state, RBNode *node)
{
    int i;

    if (node == NULL) return;
    if (node->level > 0) {
        for (i = 0; i < node->count; ++i) {
            rb_free_tree(state, node->entries[i].child);
        }
    }
    rb_deallocate(state, node, sizeof(*node));
}