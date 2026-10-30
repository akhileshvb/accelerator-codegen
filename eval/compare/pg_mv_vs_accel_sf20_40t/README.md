# TPC-H SF20 accelerators vs MVs + pg_ivm, PostgreSQL 17, 40 threads each

Figure `data/speedup_vs_mv_40t.pdf`. Server (one container, same settings for both systems) on socket 0
(CPUs 0-19,40-59 = 40 threads), clients on socket 1. 8 closed-loop readers x (leader + 4 parallel workers);
one writer commits the dbgen refresh functions (RF1/RF2 update sets + 6 point writes) every 120 / 60 / 30 s
(~1,250 / 2,500 / 5,000 rows/s offered), or none (read-only). Speedup per query = median in-window MV read /
median in-window accelerator read. Run 2026-10-20, one repetition (rep 0).

The MV writer could not keep up at any offered rate: it achieved ~335 rows/s (accelerators: 1,169 / 2,503 / 5,008).
See `data/summary.json` / `data/logs/plot_r0.log`.

Regenerate the figure from the committed data:

    python3 scripts/plot.py data OUT_DIR

## Runs

Repo code: commit 16f08bd6 plus the `eval/compare/mv_defs.py` change committed with this directory
(`data/sweep_*/.../meta/code.diff`). Paths inside the scripts are the original machine's (`env.sh`: `W`, `REPO`).

| Step | Script | Output in `data/` |
|---|---|---|
| Load SF20 (official dbgen) into PG 17 on tmpfs, PKs/indexes, ANALYZE | `scripts/load_sf20.sh` | `logs/load.log` |
| Server for all runs (image `accel-pg40:17`, socket 0) | `scripts/start_server.sh` | |
| MV design selection: v1 read latency + writer-alone cost, v2 candidates for Q2/3/10/15/21/22 | `scripts/phase1b.sh` (`mv_readlat.py`, `writer_summary.py`) | `phase1/` |
| MV sweep (pg_ivm IMMVs; Q21/Q22 refreshed views) | `scripts/sweep.sh mv 0 1` | `sweep_mv/`, `logs/sweep_mv_r0.log` |
| Drop views, register accelerators, 40-thread GUCs, accelerator sweep | `scripts/accel_phase.sh 0 1` | `sweep_accel/`, `logs/{register,sweep_accel_r0}.log` |
| Figure | `scripts/plot.py` | `speedup_vs_mv_40t.{csv,pdf}`, `summary.json` |

Not scripted at the time (reconstructed, check before re-running):
- `accel-pg40:17`: `docker commit` of the `load_sf20.sh` container (postgres:17) after installing
  `postgresql-17-pg-ivm` (pg_ivm 1.15, as `scripts/ensure_pg_ivm.sh` does) and copying the Release build's
  `accelerator_codegen_pg.so` / `libaccelerator_codegen.so` into `/usr/lib/postgresql/17/lib`.
- `backup/base_sf20.tar`: tar of the loaded data dir, restored into `/tmp/pg40/data` before a sweep.
- v1 views (`phase1/create_v1.log`, `phase1/check_v1.log`), as phase1b's step C with `MV_DESIGN=v1`:
  `mv_defs.py create --schema public --jobs 3 --workers 39`, then
  `mv_defs.py check --schema public --sets 4 --edges --workers 4 --out phase1/draws`.

Per-run JSONs are gzipped (`plot.py` reads `.json` or `.json.gz`). The `meta/` copies of repo sources
(`run_sql_backend.py`, `aux_compare.cpp`, ...) are left out: they are commit 16f08bd6 + `meta/code.diff`.
