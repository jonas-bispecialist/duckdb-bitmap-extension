# Chunked BITSTRING Bitmap Strategy

Date: 2026-04-25

This note captures the follow-up benchmark work after the Roaring BLOB bottleneck analysis. The goal was to find a simpler bitmap strategy that keeps active-set counts fast, works well when users filter large datasets down to useful UI-sized results, and avoids overfitting to one TPC-H workload.

## Product Assumption

FeatherPeek should count active sets quickly before fetching rows.

The row retrieval path has a hard practical limit around `1M` rows, and the useful interactive case is usually much smaller, often thousands to tens of thousands of rows. Large result sets are better handled as "too broad" UI states, aggregate previews, or explicit export workflows.

This changes the optimization target:

- optimize active counts and over-limit checks first,
- keep active filter state reusable,
- fetch rows only after the active count is acceptable,
- do not optimize for broad full-table export as the primary path.

## Candidate Representation

Use DuckDB-native chunked `BITSTRING` postings with `2048` fact rows per bitmap chunk.

Conceptually:

```text
field/value       chunk_id   bits
returnflag = R    0          BITSTRING for rid 0..2047
returnflag = R    1          BITSTRING for rid 2048..4095
returnflag = N    0          BITSTRING for rid 0..2047
linestatus = O    0          BITSTRING for rid 0..2047
```

Mapping:

```text
chunk_id = rid / 2048
offset   = rid % 2048
rid      = chunk_id * 2048 + offset
```

Build shape:

```sql
CREATE TABLE bitmap_returnflag AS
SELECT
    l_returnflag AS value,
    CAST(floor(rid / 2048) AS UBIGINT) AS chunk_id,
    bitstring_agg(
        CAST(rid % 2048 AS UBIGINT),
        0::UBIGINT,
        2047::UBIGINT
    ) AS bits
FROM li
GROUP BY 1, 2
ORDER BY 1, 2;
```

Count shape for multi-field filters:

```sql
WITH ret AS (
    SELECT chunk_id, bit_or(bits) AS bits
    FROM bitmap_returnflag
    WHERE value IN ('R', 'N')
    GROUP BY chunk_id
),
lin AS (
    SELECT chunk_id, bits
    FROM bitmap_linestatus
    WHERE value = 'O'
),
ship AS (
    SELECT chunk_id, bit_or(bits) AS bits
    FROM bitmap_shipmode
    WHERE value IN ('AIR', 'RAIL')
    GROUP BY chunk_id
)
SELECT SUM(bit_count(ret.bits & lin.bits & ship.bits)) AS active_row_count
FROM ret
JOIN lin USING (chunk_id)
JOIN ship USING (chunk_id);
```

Rules are unchanged:

- OR selected values inside one field,
- AND field-level bitmaps across fields,
- count set bits without materializing row ids.

## Why `2048`

`2048` beat `8192` in the local SF10 benchmarks.

At SF10, around `60M` fact rows:

```text
2048 chunks:  ~29,291 chunks per dense value
8192 chunks:   ~7,323 chunks per dense value
```

`8192` creates fewer rows, but each bitstring is larger. For sparse/scattered values, `2048` stores less payload and was faster in the measured count paths.

For the sparse date posting used in the benchmark:

```text
8192: 5,409 chunks, 5.54 MB payload
2048: 8,391 chunks, 2.15 MB payload
```

This does not mean `2048` is universally optimal. It is the best current default because it is fast for counts, helps sparse filters, and aligns with DuckDB's normal execution vector size. The chunk size should remain configurable for future larger datasets.

## Metadata Table

Keep per-value metadata beside each bitmap field:

```sql
CREATE TABLE bitmap_returnflag_meta AS
SELECT
    value,
    COUNT(*) AS chunk_count,
    SUM(bit_count(bits)) AS row_count,
    MIN(chunk_id) AS min_chunk_id,
    MAX(chunk_id) AS max_chunk_id,
    SUM(octet_length(bits)) AS payload_bytes
FROM bitmap_returnflag
GROUP BY 1;
```

This enables cheap counts for single-field filters on mutually exclusive values:

```sql
SELECT SUM(row_count)
FROM bitmap_shipmode_meta
WHERE value IN ('AIR', 'RAIL', 'TRUCK');
```

This is valid when the field is single-valued per fact row. It should not be used for multi-field intersections.

Metadata also helps query planning:

- choose the most selective field first,
- skip impossible value groups,
- identify broad filters before doing expensive retrieval,
- estimate whether active row count is likely above UI limits.

## Active Chunk Materialization

When a UI filter state will be reused, materialize active chunks once:

```sql
CREATE TEMP TABLE active_chunks AS
WITH ret AS (...),
lin AS (...),
ship AS (...)
SELECT
    ret.chunk_id,
    ret.bits & lin.bits & ship.bits AS bits
FROM ret
JOIN lin USING (chunk_id)
JOIN ship USING (chunk_id)
WHERE bit_count(ret.bits & lin.bits & ship.bits) > 0
ORDER BY chunk_id;
```

Then count is cheap:

```sql
SELECT SUM(bit_count(bits)) AS active_row_count
FROM active_chunks;
```

The same `active_chunks` table can feed later facets, aggregates, row-id enumeration, and page fetches.

## Benchmark Summary

Local SF10 database:

- DuckDB CLI `v1.5.0`
- fact rows: `59,986,052`
- chunk size: `2048`
- tested against existing Roaring BLOB extension and native SQL

### Count Workloads

Multi-field count:

```sql
l_returnflag IN ('R', 'N')
AND l_linestatus = 'O'
AND l_shipmode IN ('AIR', 'RAIL')
```

Result: `8,568,190` rows.

```text
Native SQL:                    ~187 ms
Roaring BLOB bitmap:           ~139 ms
Per-field 2048 BITSTRING:      ~127 ms
Materialized active_chunks:     ~64-68 ms
```

Single-field count:

```sql
l_shipmode IN ('AIR', 'RAIL', 'TRUCK')
```

Result: `25,705,557` rows.

```text
Native SQL:                    ~196 ms
Roaring BLOB bitmap:           ~130 ms
Per-field 2048 BITSTRING:       ~82 ms
Metadata-only count:            ~44 ms
```

Sparse count:

```sql
l_shipdate = DATE '1998-10-14'
AND l_returnflag = 'N'
```

Result: `9,967` rows.

```text
Native SQL:                    ~117 ms
Per-field 2048 BITSTRING:       ~67 ms
Unified 2048 BITSTRING:         ~64 ms
Materialized active_chunks:     ~47-51 ms
```

### Retrieval Workloads

Fetching the same `9,967` sparse rows:

```text
Native SQL fetch:                          ~150 ms
active_chunks -> expand ids -> join li:    ~282 ms
precomputed selected_rids -> join li:      ~219 ms
precomputed selected_rids -> IN subquery:  ~226 ms
range join + get_bit:                       ~34 s
```

Native SQL still wins for fact-row retrieval when the original predicates are available. The pure-SQL bitmap retrieval shapes still make DuckDB join generated row ids back to the fact table, and DuckDB does not treat chunked bitstrings as internal scan selection vectors.

## Physical Layout Recommendation

Use one logical bitmap abstraction in the application, but store physical postings per field:

```text
bitmap_returnflag(value, chunk_id, bits)
bitmap_linestatus(value, chunk_id, bits)
bitmap_shipmode(value, chunk_id, bits)
bitmap_shipdate(value, chunk_id, bits)
```

The unified table shape is simpler:

```text
bitmap_postings(field_id, value_id, chunk_id, bits)
```

but it was slower in the dense multi-field benchmark. Separate per-field tables let DuckDB scan smaller tables and prune more simply. If a unified table is used for operational simplicity, use integer `field_id`/`value_id` keys and physically order by:

```text
field_id, value_id, chunk_id
```

## Current Recommendation

Use chunked `BITSTRING` for:

- active-row counts,
- count-before-fetch over-limit checks,
- selected-set cardinality,
- repeated UI filter state via `active_chunks`,
- metadata-only single-field counts where values are mutually exclusive.

Use native DuckDB SQL for:

- first-page fact-row fetch when original predicates are available,
- broad row retrieval,
- full export,
- projected fact-column reads.

Keep the current Roaring BLOB extension as useful comparison and possible sparse/high-cardinality fallback, but the next count-focused prototype should be SQL-only chunked `BITSTRING`.

## Further Improvements Without Overfitting

These are general improvements, not TPC-H-specific tricks:

1. Keep chunk size configurable.

   Default to `2048`, but record enough metadata to compare `4096`, `8192`, and larger chunks on real user data. The right size depends on value distribution and row count.

2. Store metadata for every posting.

   Use `row_count`, `chunk_count`, `min_chunk_id`, `max_chunk_id`, and `payload_bytes` for cheap counts, planning, and early broad-filter detection.

3. Drive intersections from the most selective field.

   For sparse filters, start from the smallest `chunk_count` field/value set and join broader fields into it. Avoid scanning broad postings first when a narrow posting is available.

4. Avoid `bit_or` for single-value filters.

   A single selected value can read `chunk_id, bits` directly. Only multi-value filters need `bit_or(bits) GROUP BY chunk_id`.

5. Materialize active chunks for reusable UI state.

   Do not recompute the same active bitmap for count, facets, row preview, and aggregates. Build `active_chunks` once per filter state.

6. Keep native SQL row fetch as the default.

   Bitmap row-id retrieval should be used only when native predicates are unavailable or when a precomputed active set is reused heavily.

7. Add a small row-id enumeration table function only if needed.

   Pure SQL `range(0, 2048) + get_bit(...)` is too slow. A table function that streams set-bit offsets from active chunks could make row-id generation cheaper, but it still will not solve fact-column retrieval by itself.

8. Benchmark facets and aggregates over `active_chunks`.

   The strongest next use case is not row fetch; it is repeated active-state operations. Facet counts and aggregate previews may benefit from avoiding repeated fact-table predicate scans.

9. Keep physical per-field tables unless unified storage proves necessary.

   Per-field tables were faster in the benchmark. A unified logical abstraction can still hide this from the application.

## Open Questions

- How large do real user datasets get before `2048` creates too many posting rows?
- Do real filter values cluster by row order, or are they scattered like TPC-H `shipdate`?
- Can `active_chunks` improve facet counts enough to replace native SQL group-by for common UI flows?
- Does a custom set-bit enumeration table function make first-page retrieval competitive, or does fact-table readback remain dominant?
- Should very sparse/high-cardinality fields stay on Roaring while dense/medium fields use chunked `BITSTRING`?
