# Bitmap Performance Bottlenecks

Date: 2026-04-25

This note summarizes what we observed when comparing the DuckDB bitmap-extension path against plain DuckDB SQL. The important distinction is that the bitmap path is good at computing matching row-id sets, but it does not automatically make DuckDB fetch fact rows more efficiently.

## Summary

Bitmap currently helps most for count-only queries where the result can stay in bitmap space.

Bitmap is weak for row retrieval because the current SQL shape expands row ids and joins back to the fact table. DuckDB still scans the fact table in those plans, so the bitmap path adds work instead of replacing the scan.

DuckDB is also a strong baseline. It scans compressed column vectors very efficiently, especially for low-cardinality columns such as flags/status/modes.

Follow-up benchmark note: [Chunked BITSTRING Bitmap Strategy](chunked-bitstring-bitmap-strategy.md) documents a SQL-only `BITSTRING` alternative that improves count paths and keeps native DuckDB SQL as the default row-fetch path.

## Multi-Field Count

Example:

```sql
SELECT COUNT(*)
FROM li
WHERE l_returnflag IN ('R', 'N')
  AND l_linestatus = 'O'
  AND l_shipmode IN ('AIR', 'RAIL');
```

DuckDB version:

- Scans the relevant columns from the fact table.
- Applies vectorized predicates over compressed column data.
- Counts matching rows directly.

Bitmap version:

- Reads posting bitmaps for selected values.
- ORs values within each field.
- ANDs field-level bitmaps.
- Returns only bitmap cardinality.

Why bitmap is not dramatically faster:

- Selected SF10 postings were large, around `7.5 MB` each.
- The count touched about five postings, so it still read/deserialized roughly `37.5 MB` of bitmap payload.
- The extension path still has DuckDB aggregate/UDF overhead.
- DuckDB's native scan over low-cardinality columns is already very optimized.

Observed SF10 result after `bm_count_and_agg`:

```text
DuckDB SQL:  ~196 ms
Bitmap:      ~143 ms
```

Bitmap is faster here, but not by an order of magnitude.

## Single-Field Count

DuckDB version:

- Scans one filtered column.
- Applies a simple `IN (...)` predicate.
- Counts matching rows.

Bitmap version:

- Reads selected value postings.
- ORs them together.
- Returns bitmap cardinality.

Why bitmap does better:

- The operation is mostly a bitmap union plus cardinality.
- No fact-row retrieval is needed.
- Avoids scanning the fact table for the count.

Observed SF10 result:

```text
DuckDB SQL:  ~210 ms
Bitmap:      ~138 ms
```

This is a good bitmap use case, though still affected by BLOB read/deserialization overhead.

## Facet Counts

Example: after an active filter, count available rows per `shipmode`.

DuckDB version:

- Scans fact data with visible predicates.
- Groups by the facet column.
- Uses DuckDB's vectorized group-by implementation.

Bitmap version:

- Builds the active bitmap.
- For each candidate facet value, loads that value's bitmap.
- Computes intersection cardinality against the active bitmap.

Why bitmap was slower:

- It repeats bitmap work per facet value.
- Each candidate value may require BLOB read/deserialization.
- The current path does not cache decoded bitmaps.
- For low-cardinality columns, DuckDB's native group-by is very fast.

Observed SF10 result:

```text
DuckDB SQL:  ~167 ms
Bitmap:      ~283 ms
```

Facet counts likely need decoded bitmap caching or a more specialized multi-facet aggregate to become attractive.

## First-Page Row Fetch

Example:

```sql
SELECT rid, l_returnflag, l_linestatus, l_shipmode, l_shipdate
FROM li
WHERE ...
ORDER BY rid
LIMIT 10000;
```

DuckDB version:

- Scans/filter fact table columns.
- Stops once enough ordered rows are produced, depending on plan shape and ordering.
- Uses vectorized execution over fact columns directly.

Bitmap version:

- Computes active bitmap.
- Expands first `10000` row ids with `bm_to_rows(bitmap, 10000, -1)`.
- Joins those row ids back to `li`.
- Reads projected fact columns through the join.

Why bitmap is slow:

- Row retrieval leaves bitmap space.
- The current extension does not push a selection mask into DuckDB's table scan.
- DuckDB sees row ids as a relation and plans a join.
- The join/readback overhead dominates the saved predicate work.

Observed SF10 multi-field first-page fetch:

```text
DuckDB SQL:  ~90 ms
Bitmap:      ~338 ms
```

The bitmap path got better after `bm_and_agg`, but row fetch is still much slower than plain SQL.

## Full Row Fetch / Export

DuckDB version:

- Scans the fact table once.
- Applies predicates.
- Writes all matching projected rows.

Bitmap version:

- Computes active bitmap.
- Expands all matching row ids.
- Joins row ids back to the fact table.
- Writes all matching projected rows.

Why bitmap is slow:

- For broad filters, the matching set is large.
- Expanding hundreds of thousands or millions of row ids is expensive.
- Joining those ids back to the fact table adds work.
- DuckDB still needs to read the projected fact columns.
- `WHERE rid IN (...)` does not become direct storage-aware row lookup; DuckDB plans it as a semi/hash join plus fact scan.

Observed SF1 full-fetch result:

```text
Multi-field all rows:
DuckDB SQL:  ~456 ms for 858,642 rows
Bitmap:      ~720 ms

Single-field all rows:
DuckDB SQL:  ~552 ms for 2,571,586 rows
Bitmap:      ~1,570 ms
```

Full export should stay plain DuckDB SQL for now.

## Row-ID Export Only

Bitmap version:

- Computes the active bitmap.
- Expands row ids through `bm_to_rows`.
- Counts or exports the row-id list.

Why this is slow at scale:

- `bm_to_rows` materializes a list of row ids.
- Full expansion at SF10 took many seconds for millions of ids.
- This is not equivalent to counting; cardinality is cheap, expansion is not.

Observed SF10 row-id materialization workload:

```text
Bitmap row-id expansion/count: ~15 s
```

This path needs a streaming table function or should be avoided unless row ids are genuinely required.

## Root Causes

The main bottlenecks are:

- Repeated BLOB reads from DuckDB posting tables.
- Repeated Roaring deserialization.
- Intermediate bitmap serialization/deserialization in SQL-shaped plans.
- No decoded bitmap cache.
- No storage-aware fact-table scan using bitmap selection masks.
- Large dense low-cardinality postings that Roaring stores close to bitset size.
- DuckDB's native scans are already very fast for the tested columns.

## Practical Guidance

Use bitmap for:

- active-row counts,
- selected-set cardinality,
- repeated UI filter state if decoded bitmaps or chunked active bitstrings are cached/materialized,
- maybe facet counts after adding cache/specialized helpers.

Use DuckDB SQL for:

- full row fetch,
- broad exports,
- first-page fetch when original predicates are available,
- high-cardinality fields unless indexing policy says otherwise.

Best next simplifications:

- Prototype SQL-only chunked `BITSTRING` postings with `2048` rows per chunk for count paths.
- Store per-value metadata such as `row_count`, `chunk_count`, and chunk range for cheap single-field counts and planning.
- Materialize temporary `active_chunks` when a filtered subset is reused for count, facets, aggregates, or row preview.
- Keep native SQL row fetch as the default unless row predicates are not expressible in SQL.
- Keep decoded Roaring caches or extension helpers as a comparison/fallback, not the only count strategy.
- Benchmark facet counts and aggregate previews over `active_chunks`; those are more promising next targets than row fetch.
