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

```sql
SELECT rtree_bulk_load(
  'idx_features',
  'SELECT id, minx, miny, maxx, maxy FROM feature_bounds',
  'str',
  268435456
);
```

The general source query must be a single read-only query returning five
columns in this exact order: integer ID, minimum X, minimum Y, maximum X,
maximum Y. IDs must be unique. Bounds must be numeric, finite, ordered, and
representable as 32-bit floats. The function returns the number of indexed
rows.

The SpatiaLite convenience entry point derives those bounds from a geometry
column and the rowid:

```sql
SELECT rtree_bulk_load_spatialite(
  'idx_roads_geom', 'roads', 'geom', 'str', 268435456
);
```

It calls `MbrMinX`, `MbrMinY`, `MbrMaxX`, and `MbrMaxY` already registered on
that SQLite connection. Rows whose `MbrMinX` is `NULL` are skipped. It does not
link against SpatiaLite.

The algorithm is optional and defaults to `str`; accepted values are `str` and
`rstar`, case-insensitively. `max_bytes` defaults to 256 MiB; `0` disables the
limit. The limit accounts for the extension's tracked allocations and the
RStar builder's estimated memory, not SQLite's internal statement/page caches.
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