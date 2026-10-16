# Performance runbook (SF20; basis of the SF3000 runbook)

Compares plain DuckDB with the C++ accelerators, in two trees: `baseline` (`/home/akhilvb/accel-baseline`, detached
at 7f5bff8f, the code behind `eval/compare/results/tpch_perf_sf20_20261012_222153.log`) and `new` (`skel/perf`, the
skeleton). Verus kits are not part of it.

## Build (Release, once per tree)

```
# baseline: its sibling DuckDB fork, and the old Verus libraries (its aux_compare takes the baseline query text from the kits)
cd /home/akhilvb/accel-baseline
ACCEL_DUCKDB_ROOT=/home/akhilvb/accelerator-codegen-duckdb cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCEL_CODEGEN_ENGINE=duckdb -DACCEL_SEC_EDGAR_COPTS=-march=native -DACCEL_STATELESS_COPTS=-march=native \
  -DACCEL_VERUS_LIB_DIR=/home/akhilvb/accelerator-codegen/build-verus
ninja -C build-release -j12 aux_compare

# new
cd /home/akhilvb/accel-perf
ACCEL_DUCKDB_ROOT=/home/akhilvb/duckdb-dbcapture cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCEL_CODEGEN_ENGINE=duckdb -DACCEL_SEC_EDGAR_COPTS=-march=native -DACCEL_STATELESS_COPTS=-march=native -DACCEL_BASE_COPTS=-march=native
ninja -C build-release -j12 aux_compare
```

`build-release` is a symlink to `build` (create the directory first). A full build takes ~25 min on a loaded machine.

## Run

On a quiet machine, in tmux (the scripts refuse when the 1-minute load is >= 4; `--force` / `PERF_FORCE=1` overrides):

```
tmux new -d -s perf 'scripts/perf/run_all_sf20.sh 32-39 /flash1/akhilvb/perf/sf20_$(date +%Y%m%d)'
tmux attach -t perf
```

This runs, for `baseline` then `new`, the read sweep and QphH@20; results in `<out>/<tree>/{read,qphh}`, log in `<out>/all.log`.
Single steps:

```
scripts/perf/run_sf20_read.sh <new|baseline> <cores> <out>            # SF=20 REPEATS=5 SETS=8 QUERIES="1 .. 22"
scripts/perf/run_qphh.sh      <new|baseline> <sf> <cores> <out>       # RUNS=3 STREAMS=3
```

- Read sweep: per query, a fresh process on a fresh copy of the database builds the accelerator and answers `REPEATS` random
  parameter draws x `SETS` parameter sets; prints speedup over DuckDB as median [min-max] over the draws, mismatches, fallbacks.
- QphH: per system (DuckDB, then the 22 accelerators in one process) a fresh copy of the database (~5 GB at SF20, in
  `$WORK`, default `/flash1/akhilvb/tpch/work`, deleted afterwards), build ("load"), then `RUNS` x (power test with RF1/RF2,
  throughput test with `STREAMS` streams and a refresh stream). Reports Power@Size, Throughput@Size, QphH@Size, per-query
  power times, RF times, `power_mismatches` (accelerator answers vs DuckDB in the power test; must be empty) and `errors`.
  The update sets come from `RF_DIR` (default `/flash1/akhilvb/tpch/refresh_sf<SF>`, `scripts/gen_tpch_refresh_sets.sh`).
  Re-summarize with `python3 eval/compare/run_compare.py summary <out>`.
- The new tree's DuckDB baseline text is `workloads/tpch/q<N>.sql` (`--baseline-sql-dir`); the baseline tree uses its
  Verus kits' copy of the same text.

## Equal core budget and knobs

`<cores>` is a taskset list; the scripts set every thread knob to its size N (all can be overridden in the environment
before calling; `MAINT_THREADS` overrides maintenance only):

| Knob | Meaning | Set to |
| --- | --- | --- |
| `taskset -c <cores>` | CPUs of the process | cores |
| `ACCEL_COMPARE_DUCKDB_THREADS` (`--duckdb-threads`) | DuckDB `threads`: baseline queries, build and maintenance scans | N |
| `ACCEL_AUX_THREADS` | accelerator thread budget (default: affinity-mask size) | N |
| `ACCEL_AUX_READ_THREADS` | cap for intra-query parallelism in `Execute` | N |
| `ACCEL_AUX_BUILD_THREADS` | cap for builds | N |
| `ACCEL_AUX_MAINT_THREADS` | cap for maintenance fork-join, within one accelerator and across the accelerators of one commit (default 1 = serial) | N (`MAINT_THREADS`) |

Postgres equivalents (GUCs): `accel.threads`, `accel.read_threads`, `accel.build_threads`, `accel.maint_threads` (0 =
the `accel.threads` budget; maintenance 0 = one); MySQL: `accel_threads`. The baseline tree ignores the `ACCEL_AUX_*`
phase caps (its reads and maintenance are single-threaded), which is the point of the comparison.

## Expected cost

SF20 numbers from the 10/12 run (40 cores, busy machine); with 8 cores expect 2-3x on builds.

- QphH@20 per tree: DuckDB 3 runs ~2 min; accelerators: build ~11 min (22 accelerators, ~660 s at 40 threads) + 3 runs
  ~1-2 min; plus the 5 GB copies. About 20-35 min per tree.
- Read sweep per tree: 22 queries x (copy + build + 5 draws); about 1-1.5 h.
- All four steps: roughly 3-4 h.

## Memory

One SF20 process with all 22 accelerators: measured with the old tree 34-37 GB RSS after build, peak 36-43 GB over three
runs (DuckDB `memory_limit` 16 GB included; state grows with the refreshes). SF1 on the new tree peaks at 2.2 GB RSS
(x20 = 44 GB upper bound). Needs ~45 GB free RAM; note that /tmp is RAM-backed and counted against it.
The audit's per-query state sizes (`git show skel/audit:docs/sf3000-duckdb-audit.md`) sum to ~1.2 GB per SF unit, i.e. ~23 GB
of state at SF20 on top of the database.

## Checks after a run

- `mismatches 0` in every read line; `hits` = all cases, `fallbacks 0`.
- QphH: no `MISMATCH` / `ERRORS` in the run lines; `throughput_fallbacks` 0 in the JSON.
- Compare per-run `rss_mb_peak` with the memory section.
