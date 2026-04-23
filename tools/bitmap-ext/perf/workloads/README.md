# Workload SQL

Each `.sql` file in this directory is a single facet-like benchmark query.

The perf runner materializes each query through `COPY (...) TO '<temp>.csv'` so latency reflects execution plus result production.

`prepare-tpch.sql` is the local TPC-H bootstrap script used to create the benchmark database.
