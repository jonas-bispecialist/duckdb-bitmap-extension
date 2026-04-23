# FeatherPeek Filtering Discussion Summary

## Goal
Make large-table filtering and facet interactions fast (60M+ rows), especially for low-cardinality filter columns, while keeping export to Excel practical (up to ~1M rows).

## Key Findings

1. `LIMIT 10` is not enough to guarantee speed.
- If the query does `DISTINCT`, `GROUP BY`, or `ORDER BY`, DuckDB still does substantial work before applying `LIMIT`.

2. DuckDB is columnar, but that does not mean `DISTINCT A` is free.
- It reads column segments across row groups.
- Low cardinality helps aggregation cost, but scanning can still be large on very big tables.

3. Traditional indexes do not solve this core UX problem.
- They help selective predicates (`A = x`) more than repeated global facet computations (`DISTINCT A` under changing filter context).

4. BI tools (Power BI/Qlik) feel faster because they use precomputed structures.
- Dictionary/symbol encoding.
- Inverted/bitmap indexes.
- Cached filter state and fast set operations.

## Proposed Runtime Model

1. Keep the fact table in DuckDB.
2. For each chosen filter field, build:
- Distinct dictionary (`value <-> id`)
- Posting/bitmap per distinct value (`id -> row set`)
3. Maintain a candidate row set from active filters using bitmap set algebra:
- OR within one field selection
- AND across fields
4. Use bitmap count for active row count (no fact-table hit needed).
5. For export, fetch fact rows by selected row ids in batches.

## Important Clarification

- The expensive step is building postings/bitmaps initially (or when first enabling a filter field).
- After that, interactive filtering and row counting can be very fast.

## DuckDB Extension Conclusion

1. Building a DuckDB extension for bitmap operations is a valid direction.
2. Existing DuckDB options found are not a full drop-in for exact OLAP-style bitmap workflow:
- `BITSTRING` functions exist (primitive bit operations).
- `bitfilters` extension is probabilistic/approximate, not exact roaring-style filter state.
3. A custom extension (preferably C++) could expose exact bitmap ops and keep computation in-engine, reducing C# <-> DuckDB transfer overhead.
4. Suggested extension shape:
- Store bitmap payloads as `BLOB`
- Add scalar/table functions like:
  - `bm_and`, `bm_or`, `bm_andnot`
  - `bm_count`
  - `bm_contains`
  - `bm_to_rows`

## Practical Recommendation

1. Start with C# bitmap implementation first (fastest to validate product UX).
2. Measure:
- Facet latency
- Filter-change latency
- Active-row count latency
- Export throughput
3. Move to DuckDB extension only if profiling shows data transfer/marshalling is the main bottleneck.
4. Keep architecture incremental; do not redesign full storage model until metrics justify it.
