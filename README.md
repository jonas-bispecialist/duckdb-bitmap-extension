# FeatherPeek DuckDB Bitmap Extension — Starting Point

## Goal

Build a **small, local-only DuckDB extension in C#** that supports FeatherPeek’s bitmap-based filter workflow.

Primary purpose:

- keep DuckDB as the fact-table engine
- keep bitmap algebra close to SQL
- avoid repeated large fact-table scans during interactive filtering
- support fast row counts and row-id expansion for export

This is **not** a general-purpose extension and **not** a publishable package.

---

## Scope Assumptions

- own use only
- Windows first
- one frozen DuckDB version
- one frozen .NET version
- loaded from local disk only
- unsigned extension is acceptable
- C# host application will control startup and loading
- no need to support arbitrary user-installed DuckDB versions

This is a major simplification and should be treated as a feature, not a limitation.

---

## Recommendation Summary

### Use this architecture

- **DuckDB fact table** stays normal
- **Bitmap dictionaries and postings** stored in DuckDB tables as metadata/assets
- **Bitmap algebra** lives in a small C# DuckDB extension
- **Bitmap build pipeline** initially lives in normal C# application code, not in the extension
- **UI orchestration** stays in FeatherPeek backend/service code

### Do not start with

- planner rewrites
- optimizer hooks
- aggregate functions
- update-heavy bitmap maintenance
- automatic query substitution
- custom storage engine work
- cross-platform packaging

### First success target

A user changes filters and FeatherPeek can:

1. combine bitmaps fast
2. return active row count fast
3. expand active row ids in batches
4. fetch fact rows for export

That is enough to prove product value.

---

## Why this split is the right starting point

Your current design direction is already incremental:

- per-column search/filter first
- persistent per-column structures
- no premature full storage redesign
- validate UX before building a larger engine layer

That matches a small bitmap extension extremely well.

The expensive part is initial bitmap creation. Interactive use should then be dominated by bitmap set algebra and row-id expansion, not by rescanning the fact table. fileciteturn6file0L5-L12 fileciteturn6file0L19-L36

---

## Technical Foundation

DuckDB now has **DuckDB.ExtensionKit** for building extensions in C#, based on the stable C Extension API and .NET Native AOT. It supports defining scalar and table functions and produces a native DuckDB extension binary. DuckDB also requires custom extensions to match the exact DuckDB version and platform, and unsigned local extensions require `allow_unsigned_extensions`. citeturn295980search5turn222804search0turn295980search1

For this project, those constraints are acceptable because version and platform can be frozen.

---

## Recommended Boundary: Extension vs App Code

## Put inside the extension

- exact bitmap binary format handling
- bitmap deserialization/serialization
- bitmap set operations
- bitmap cardinality/count
- membership checks
- table function to emit row ids from a bitmap
- optional helper functions for diagnostics

## Keep outside the extension at first

- deciding which columns become bitmap-enabled
- extracting distinct values from source data
- building dictionary rows
- building posting lists
- persisting bitmap blobs into metadata tables
- cache policy
- UI state management
- export batching
- fallback SQL behavior when no bitmap exists

This keeps the extension small and testable.

---

## Core Design Decision

### Persist bitmap payloads as `BLOB`

Use `BLOB` as the SQL type for stored bitmap payloads.

Why:

- simple extension surface
- decouples SQL layer from internal bitmap structure
- easy versioning of payload format
- easy to store in metadata tables
- easy to swap bitmap implementation later if needed

### Strong recommendation

Add a small binary header to every payload:

- magic bytes
- format version
- bitmap kind
- reserved flags

This avoids silent corruption and makes future migration possible.

---

## Strong recommendation on row identity

Do **not** rely on physical row order or unstable implicit row identifiers.

Create and persist a stable synthetic row id for the fact table, for example:

- `fp_row_id BIGINT`

Every bitmap should reference this stable row id domain.

Why:

- safer across reloads/rebuilds
- easier export join-back
- easier testing
- avoids ambiguity if source ordering changes

---

## Initial SQL Surface

Start with a very small function set.

## Phase 1 scalar functions

### `bm_and(left BLOB, right BLOB) -> BLOB`
Intersect two bitmaps.

### `bm_or(left BLOB, right BLOB) -> BLOB`
Union two bitmaps.

### `bm_andnot(left BLOB, right BLOB) -> BLOB`
Subtract right from left.

### `bm_xor(left BLOB, right BLOB) -> BLOB`
Optional but cheap and useful for debugging.

### `bm_count(bitmap BLOB) -> UBIGINT`
Return bitmap cardinality.

### `bm_contains(bitmap BLOB, row_id BIGINT) -> BOOLEAN`
Useful for diagnostics and tests.

### `bm_is_empty(bitmap BLOB) -> BOOLEAN`
Useful shortcut for UI and guard paths.

### `bm_debug_info(bitmap BLOB) -> VARCHAR`
Optional. Return a compact human-readable summary for development.

---

## Phase 2 table function

### `bm_to_rows(bitmap BLOB)`
Return rows like:

| column | type |
|---|---|
| `row_id` | `BIGINT` |

This is the key bridge back into normal SQL.

Example use:

```sql
SELECT f.*
FROM fact_sales f
JOIN bm_to_rows(?) b
  ON f.fp_row_id = b.row_id
ORDER BY f.fp_row_id
LIMIT 10000;
```

This function matters more than most extra scalar helpers.

---

## Phase 3 optional helpers

Only add these after Phase 1 and 2 work well.

### `bm_from_rowids(list<BIGINT>) -> BLOB`
Useful for tests and some orchestration paths.

### `bm_range(start_row_id BIGINT, end_row_id BIGINT) -> BLOB`
Useful for creating full-domain or test bitmaps.

### `bm_intersects(left BLOB, right BLOB) -> BOOLEAN`
Can be a cheap optimization in some UI paths.

### `bm_count_and(left BLOB, right BLOB) -> UBIGINT`
Avoids materializing an intermediate bitmap for count-only paths.

### `bm_count_or(left BLOB, right BLOB) -> UBIGINT`
Same idea.

Do not start here. Add only if profiling shows benefit.

---

## Initial metadata tables in DuckDB

Keep metadata explicit.

## 1. Enabled bitmap fields

```sql
CREATE TABLE fp_bitmap_fields (
    source_table_name VARCHAR NOT NULL,
    field_name VARCHAR NOT NULL,
    row_id_column_name VARCHAR NOT NULL,
    build_status VARCHAR NOT NULL,
    bitmap_format_version INTEGER NOT NULL,
    distinct_value_count BIGINT,
    created_at TIMESTAMP,
    updated_at TIMESTAMP,
    PRIMARY KEY (source_table_name, field_name)
);
```

## 2. Value dictionary

```sql
CREATE TABLE fp_bitmap_dictionary (
    source_table_name VARCHAR NOT NULL,
    field_name VARCHAR NOT NULL,
    value_id BIGINT NOT NULL,
    value_text VARCHAR,
    value_type VARCHAR NOT NULL,
    row_count BIGINT,
    PRIMARY KEY (source_table_name, field_name, value_id)
);
```

## 3. Posting bitmaps

```sql
CREATE TABLE fp_bitmap_postings (
    source_table_name VARCHAR NOT NULL,
    field_name VARCHAR NOT NULL,
    value_id BIGINT NOT NULL,
    bitmap_payload BLOB NOT NULL,
    row_count BIGINT,
    payload_size_bytes BIGINT,
    PRIMARY KEY (source_table_name, field_name, value_id)
);
```

Notes:

- `value_text` is fine for V1 display needs
- typed value columns can come later
- `row_count` should be persisted for fast UI display and sanity checks
- `payload_size_bytes` helps diagnostics quickly

---

## Build flow recommendation

Do bitmap build in normal C# application code first.

## Suggested build steps

1. ensure fact table has stable `fp_row_id`
2. choose one field to enable for bitmap filtering
3. read distinct values for that field
4. assign deterministic `value_id`
5. build posting list per distinct value
6. convert posting list to bitmap payload
7. store payload in `fp_bitmap_postings`
8. store counts in dictionary and field metadata
9. verify counts against SQL truth queries

This keeps extension scope small.

---

## Query flow recommendation

## UI selection semantics

- OR within one field
- AND across fields

## Example flow

If user selects:

- `Country IN ('SE', 'NO')`
- `Status IN ('Open')`

Then app code:

1. load `Country=SE` bitmap
2. load `Country=NO` bitmap
3. `bm_or(SE, NO)`
4. load `Status=Open` bitmap
5. `bm_and(country_union, status_open)`
6. `bm_count(result)` for active row count
7. `bm_to_rows(result)` for export/sample fetch

This matches your target interaction model directly.

---

## High-level implementation plan

## Step 0 — Freeze versions

Freeze:

- DuckDB version
- DuckDB.NET version
- .NET SDK version
- Windows target runtime

Do this before coding the real extension.

Reason:

- extension binaries are version- and platform-bound
- freezing versions removes a large class of instability

---

## Step 1 — Create the smallest possible extension

Goal:

- one C# extension project
- Native AOT build
- local unsigned load
- one trivial function like `bm_debug_info` or `bm_is_empty`

Success criteria:

- C# app starts DuckDB
- config allows unsigned extension load
- extension loads from local path
- simple function executes in SQL

Do this before any bitmap logic.

---

## Step 2 — Implement payload format and scalar ops

Goal:

- choose bitmap representation
- implement serialize/deserialize
- implement `bm_and`, `bm_or`, `bm_andnot`, `bm_count`

Success criteria:

- deterministic outputs
- round-trip serialization tests pass
- count results match expected set math

Recommendation:

Keep internal bitmap implementation hidden behind an interface, even if there is only one implementation.

Example internal boundary:

```csharp
public interface IBitmapCodec
{
    byte[] Serialize(IBitmap bitmap);
    IBitmap Deserialize(ReadOnlySpan<byte> payload);
}

public interface IBitmap
{
    ulong Count { get; }
    bool Contains(long rowId);
    IBitmap And(IBitmap other);
    IBitmap Or(IBitmap other);
    IBitmap AndNot(IBitmap other);
    IEnumerable<long> EnumerateRowIds();
}
```

This is mainly to keep the rest of the extension stable.

---

## Step 3 — Implement `bm_to_rows`

Goal:

Emit row ids as a table function.

Success criteria:

- can join emitted row ids back to fact table
- can stream in chunks
- works for export/sample fetch use cases

This is the first end-to-end useful milestone.

---

## Step 4 — Build one bitmap-enabled field end to end

Goal:

Pick one low-cardinality field and make the full loop work.

Success criteria:

- build dictionary/postings
- persist payloads
- combine selections
- count active rows fast
- fetch first N matching rows via join on `bm_to_rows`

Do not enable ten fields first.

---

## Step 5 — Add measurement harness

Track at minimum:

- bitmap build time per field
- payload size per distinct value
- total payload size per field
- filter-change latency
- active-row count latency
- `bm_to_rows` throughput
- export throughput after join-back

Store test results in a simple markdown or csv log.

---

## Step 6 — Harden only after proof

Only after the workflow proves valuable:

- improve compression/representation
- add count-only helpers
- add build parallelism
- add cache policy
- add more fields
- consider moving parts of build into extension

---

## Recommended implementation posture

## Recommendation 1

Treat the extension as a **small native execution module**, not as the whole bitmap subsystem.

## Recommendation 2

Keep bitmap build orchestration in normal C# service code until metrics say otherwise.

## Recommendation 3

Prefer a stable SQL API over a clever internal implementation.

## Recommendation 4

Test correctness aggressively before optimizing.

## Recommendation 5

Version your payload format from day one.

## Recommendation 6

Make all SQL-visible functions deterministic and side-effect free.

---

## What not to do in V1

Do not do these yet:

- custom optimizer integration
- hidden automatic bitmap rewrite of user SQL
- bitmap maintenance for mutable tables
- direct dependence on internal DuckDB C++ APIs
- multi-platform shipping
- generalized full-text/global search engine
- extension-managed metadata schema evolution

These are all valid later topics, but they are not the right starting point.

---

## Suggested first milestone demo

A demo is successful if this exact scenario works:

1. load fact table with stable `fp_row_id`
2. bitmap-enable one field such as `Country`
3. store posting bitmap per distinct country
4. select two countries in UI
5. union those bitmaps
6. show active row count immediately
7. expand row ids via `bm_to_rows`
8. join back to fact table and fetch first 5,000 rows
9. export works correctly

If this works well, the architecture is good enough to continue.

---

## Suggested project layout

```text
/featherpeek
  /src
    /FeatherPeek.BitmapExtension
      FeatherPeek.BitmapExtension.csproj
      ExtensionEntrypoint.cs
      BitmapFunctions.cs
      BitmapTableFunctions.cs
      PayloadFormat.cs
      BitmapCodec.cs
      BitmapImplementation.cs
    /FeatherPeek.Backend
      BitmapBuildService.cs
      BitmapMetadataRepository.cs
      BitmapQueryService.cs
  /tests
    /FeatherPeek.BitmapExtension.Tests
    /FeatherPeek.Backend.Tests
  /docs
    duckdb-bitmap-extension-notes.md
```

---

## Example SQL shape for app integration

```sql
-- load extension once per connection/session
LOAD 'featherpeek_bitmap.duckdb_extension';

-- count active rows
SELECT bm_count(
    bm_and(
        bm_or(?, ?),
        ?
    )
);

-- fetch actual rows
SELECT f.*
FROM fact_sales f
JOIN bm_to_rows(?) r
  ON f.fp_row_id = r.row_id
ORDER BY f.fp_row_id
LIMIT 100000;
```

The app can compose these statements without needing DuckDB planner magic.

---

## Final recommendation

Start with a **minimal C# Native AOT DuckDB extension** that only does exact bitmap algebra plus `bm_to_rows`.

That is the best tradeoff for your situation because:

- it matches your current filter model
- it keeps the hard part small
- it works with a frozen DuckDB version
- it is compatible with a C# host application
- it gives you a fast path to measuring real UX value

If this proves useful, then the next step is **not** a full engine rewrite. The next step is usually just:

- better bitmap format
- more helper functions
- faster build pipeline
- selective movement of build logic closer to DuckDB

That should be the starting point.
