# SQLite RTree Bulk Loader

A loadable SQLite extension for building 2D floating-point RTree indexes. `str`
uses deterministic Sort-Tile-Recursive packing. `rstar` uses the vendored
RTree bulk builder from Even Rouault, with SQLite's shadow-table serializer.

## Build

Requirements: CMake 3.20+, a C99 compiler, and SQLite 3.31+ development headers
and library. The extension uses SQLite's loadable-extension API and does not
link to GEOS or SpatiaLite. The target is a CMake `MODULE`, with platform-native
shared-library suffixes and no SQLite link dependency. CTest links its test
executable against the host SQLite library.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix install
```

GitHub Actions builds and runs the test suite on Ubuntu, macOS, and Windows.

## SQL API

The functions are registered as `SQLITE_DIRECTONLY` because they replace
database schema objects. Enable extension loading only for trusted extension
files, then load `rtreebulkload` through the host's normal extension loader.

The functions have these signatures:

- `rtree_bulk_load(index_name, select_sql [, algorithm [, max_bytes]])`
- `rtree_bulk_load_table(index_name, source_table, geometry_column [, algorithm [, max_bytes]])`

Parameters:

- `index_name`: name of the RTree virtual table to create or replace.
- `select_sql`: a single read-only query returning five columns in this order:
  integer ID, minimum X, minimum Y, maximum X, maximum Y. IDs must be unique;
  bounds must be numeric, finite, ordered, and representable as 32-bit floats.
- `source_table` and `geometry_column`: the table and geometry column used by
  `rtree_bulk_load_table`. It uses each row's `ROWID` as the RTree entry
  ID, so the table must not be `WITHOUT ROWID`. The `ST_MinX`, `ST_MinY`,
  `ST_MaxX`, `ST_MaxY`, and `ST_IsEmpty` functions must already be registered
  on the SQLite connection. Rows with `NULL` or empty geometries, or whose
  `ST_MinX` is `NULL`, are skipped. The extension does not link against
  SpatiaLite.
- `algorithm`: optional; `str` by default, or `rstar` (case-insensitive).
- `max_bytes`: optional memory limit, defaulting to 256 MiB. `0` disables the
  limit. It accounts for the extension's tracked allocations and the RStar
  builder's estimated memory, but not SQLite's internal statement/page caches.

Both functions return the number of indexed rows.

```sql
SELECT rtree_bulk_load(
  'idx_features',
  'SELECT id, minx, miny, maxx, maxy FROM feature_bounds',
  'str'
);
```

```sql
SELECT rtree_bulk_load_table(
  'idx_roads_geom', 'roads', 'geom', 'str'
);
```

The source records must fit in memory for either algorithm. If the RStar
builder reaches the remaining budget, it serializes the packed prefix and
inserts the remaining records through SQLite's RTree virtual table.

Each call creates or replaces the named virtual table with columns
`id, minx, maxx, miny, maxy`, inside a savepoint. Any input, build, or write
error rolls back and preserves an existing target. Do not choose a target name
that is also the source table.

## Compatibility

Both algorithms write SQLite's private RTree shadow tables. SQLite documents
those tables as implementation details; this format can change between SQLite
versions. STR writes the packed node layout directly; RStar uses the vendored
serializer. Both derive page-dependent behavior from the database page size
and require IEEE binary32 floats. Validate built indexes with
`SELECT rtreecheck('index_name')`.

The upstream RTree bulk-builder source and header are vendored unchanged from
Even Rouault's repository at commit `7f687fcbf41211df6d9987d32aa0e35fc7c7ffb6`.
See the included [MIT license](vendor/sqlite_rtree_bulk_load/LICENSE).