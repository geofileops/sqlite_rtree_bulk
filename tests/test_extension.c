#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(sqlite3 *database, const char *message)
{
    fprintf(stderr, "%s: %s\n", message,
            database != NULL ? sqlite3_errmsg(database) : "no database");
    if (database != NULL) sqlite3_close(database);
    exit(EXIT_FAILURE);
}

static int query_int(sqlite3 *database, const char *sql)
{
    sqlite3_stmt *statement = NULL;
    int value;

    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        fail(database, sql);
    }
    value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

static void expect_index_ok(sqlite3 *database, const char *index_name)
{
    char sql[256];
    sqlite3_stmt *statement = NULL;

    sqlite3_snprintf((int)sizeof(sql), sql,
                     "SELECT rtreecheck('%q')", index_name);
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW ||
        sqlite3_column_text(statement, 0) == NULL ||
        sqlite3_stricmp((const char *)sqlite3_column_text(statement, 0), "ok") != 0) {
        fail(database, "rtreecheck failed");
    }
    sqlite3_finalize(statement);
}

static void expect_sql_failure(sqlite3 *database, const char *sql)
{
    char *error = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error);

    if (result == SQLITE_OK) {
        sqlite3_free(error);
        fail(database, "expected SQL failure");
    }
    sqlite3_free(error);
}

static int parse_bounds(sqlite3_value *value, double bounds[4])
{
    const char *text = (const char *)sqlite3_value_text(value);
    char *end;
    int i;

    if (text == NULL) return 0;
    for (i = 0; i < 4; ++i) {
        while (*text == ' ' || *text == '\t') ++text;
        errno = 0;
        bounds[i] = strtod(text, &end);
        if (end == text || errno == ERANGE) return 0;
        text = end;
        if (i < 3) {
            if (*text != ',') return 0;
            ++text;
        }
    }
    while (*text == ' ' || *text == '\t') ++text;
    return *text == '\0';
}

static void mbr_component(sqlite3_context *context, int argc,
                          sqlite3_value **argv)
{
    const int component = *(const int *)sqlite3_user_data(context);
    double bounds[4];

    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }
    if (!parse_bounds(argv[0], bounds)) {
        sqlite3_result_error(context, "invalid test geometry", -1);
        return;
    }
    sqlite3_result_double(context, bounds[component]);
}

static void register_test_mbr(sqlite3 *database)
{
    static const int components[4] = {0, 1, 2, 3};
    static const char *names[4] = {
        "MbrMinX", "MbrMinY", "MbrMaxX", "MbrMaxY"
    };
    int i;

    for (i = 0; i < 4; ++i) {
        if (sqlite3_create_function_v2(database, names[i], 1, SQLITE_UTF8,
                (void *)&components[i], mbr_component, NULL, NULL, NULL) != SQLITE_OK) {
            fail(database, "register test MBR function");
        }
    }
}

int main(int argc, char **argv)
{
    sqlite3 *database = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s extension-path\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (sqlite3_open(":memory:", &database) != SQLITE_OK) {
        fail(database, "open database");
    }
    if (sqlite3_enable_load_extension(database, 1) != SQLITE_OK ||
        sqlite3_load_extension(database, argv[1], NULL, NULL) != SQLITE_OK) {
        fail(database, "load extension");
    }

    if (sqlite3_exec(database,
        "CREATE TABLE source(id INTEGER, minx REAL, miny REAL, maxx REAL, maxy REAL);"
        "INSERT INTO source VALUES (1, 0, 0, 1, 1),"
        "(2, -2, -1, 2, 3), (3, 10, 10, 20, 30);",
        NULL, NULL, NULL) != SQLITE_OK) {
        fail(database, "create source data");
    }

    if (query_int(database,
        "SELECT rtree_bulk_load('idx_str',"
        "'SELECT id,minx,miny,maxx,maxy FROM source ORDER BY id')") != 3) {
        fail(database, "STR load row count");
    }
    expect_index_ok(database, "idx_str");
    if (query_int(database, "SELECT count(*) FROM idx_str") != 3 ||
        query_int(database,
            "SELECT count(*) FROM idx_str WHERE minx <= 1 AND maxx >= 0 "
            "AND miny <= 1 AND maxy >= 0") != 2) {
        fail(database, "STR contents or range query");
    }

    if (query_int(database,
        "SELECT rtree_bulk_load('idx_rstar',"
        "'SELECT id,minx,miny,maxx,maxy FROM source ORDER BY id','rstar')") != 3) {
        fail(database, "RStar load row count");
    }
    expect_index_ok(database, "idx_rstar");
    if (query_int(database, "SELECT count(*) FROM idx_rstar") != 3) {
        fail(database, "RStar contents");
    }
    if (query_int(database,
        "SELECT rtree_bulk_load('idx_rstar_fallback',"
        "'SELECT id,minx,miny,maxx,maxy FROM source ORDER BY id','rstar',512)") != 3) {
        fail(database, "RStar memory fallback row count");
    }
    expect_index_ok(database, "idx_rstar_fallback");
    if (query_int(database, "SELECT count(*) FROM idx_rstar_fallback") != 3) {
        fail(database, "RStar memory fallback contents");
    }
    if (query_int(database,
        "SELECT rtree_bulk_load('idx_rstar',"
        "'SELECT id,minx,miny,maxx,maxy FROM source WHERE id < 3','rstar')") != 2) {
        fail(database, "RStar replacement row count");
    }
    expect_index_ok(database, "idx_rstar");
    if (query_int(database, "SELECT count(*) FROM idx_rstar") != 2) {
        fail(database, "RStar replacement contents");
    }

    expect_sql_failure(database,
        "SELECT rtree_bulk_load('idx_str',"
        "'SELECT 1,0,0,1,1 UNION ALL SELECT 1,0,0,1,1')");
    if (query_int(database, "SELECT count(*) FROM idx_str") != 3) {
        fail(database, "failed rebuild did not preserve previous index");
    }
    expect_sql_failure(database,
        "SELECT rtree_bulk_load('idx_str',"
        "'SELECT id,minx,miny,maxx,maxy FROM source','str',1)");
    if (query_int(database, "SELECT count(*) FROM idx_str") != 3) {
        fail(database, "memory-limit rollback did not preserve previous index");
    }

    register_test_mbr(database);
    if (sqlite3_exec(database,
        "CREATE TABLE geom_source(geom TEXT);"
        "INSERT INTO geom_source VALUES ('0,0,1,1'),('-1,-1,2,2'),"
        "(NULL),('20,30,40,50');",
        NULL, NULL, NULL) != SQLITE_OK) {
        fail(database, "create geometry test data");
    }
    if (query_int(database,
        "SELECT rtree_bulk_load_spatialite('idx_geom','geom_source','geom','str')") != 3) {
        fail(database, "SpatiaLite-mode load row count");
    }
    expect_index_ok(database, "idx_geom");
    if (query_int(database, "SELECT count(*) FROM idx_geom") != 3 ||
        query_int(database, "SELECT count(*) FROM idx_geom WHERE id IN (1,2,4)") != 3) {
        fail(database, "SpatiaLite-mode NULL MBR filtering or rowids");
    }

    sqlite3_close(database);
    puts("all bulk-load tests passed");
    return EXIT_SUCCESS;
}