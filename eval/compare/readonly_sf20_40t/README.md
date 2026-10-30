# TPC-H SF20 read-only speedup, 40 threads, same DuckDB base

Figure `readonly_speedup_sf20_40t_samebase.pdf`. All systems run on socket 0 (CPUs 0-19,40-59, NUMA node 0) at 40 threads.
Every speedup uses one DuckDB reference per query: the 40T warm median on the PK database (`base_us` in `data/cpp_read`).

Regenerate the figure from the committed data:

    python3 plot_40t_samebase.py OUT_DIR data/warm_sweep.log

Runs (paths inside the scripts are the original machine's; edit them for a new host):

| Step | Script | Output in `data/` |
|---|---|---|
| PK database | `scripts/make_pk_db.sh` | (tpch_sf20_pk.duckdb) |
| Stateful C++ vs DuckDB (commit 29ed49f7) | `scripts/run_cpp_read_40t_pk.sh` | `cpp_read/` |
| Stateful C++ warm back-to-back latency | `scripts/warm_sweep.sh` | `warm_sweep.log` |
| Stateless C++ vs DuckDB | `scripts/run_stateless_40t_pk.sh` (`run_both_pk.sh` = Verus read + stateless) | `stateless_duckdb_sweep_t40only_sf20_*.csv` |
| GenDB / Bespoke OLAP | `scripts/run_socket0_40t.sh` (+ `bespoke_mt_allcore_repro.py`, `gendb_paper_repro.py`; run from `olap-80t/repro`) | `sf20_40t_socket0_bespoke_gendb.csv` |
| Verified stateful, Postgres 40v40 | merged by hand from the runs' CSVs | `stateful_verified_40t.csv`, `pg_40v40_readonly.csv` |
