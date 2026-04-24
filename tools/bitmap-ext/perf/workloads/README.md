# Workload SQL

Each `.sql` file in this directory is a single facet-like benchmark query.

The perf runner materializes each query through `COPY (...) TO '<temp>.csv'` so latency reflects execution plus result production.

The workload pairs cover:

- single-field count: normal `WHERE field IN (...)` vs bitmap union plus `bm_count`
- single-field fetch: normal `WHERE field IN (...)` vs `bm_to_rows(...)` joined back to `li`
- multi-field count: normal `WHERE A IN (...) AND B = ...` vs bitmap set algebra plus `bm_count`
- multi-field fetch: normal filtered fact query vs bitmap filtered row ids joined back to `li`
- facet-style counts: normal filtered `DISTINCT` vs bitmap intersections per facet value

Fetch workloads use `ORDER BY rid LIMIT 10000` to model a first page/export batch while keeping CSV output from dominating the benchmark.

`prepare-tpch-data.sql` creates the benchmark fact table. `prepare-bitmaps.sql` creates reusable bitmap postings.
