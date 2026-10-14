"""Read-only rerun of Q17/Q18 with max_parallel_workers_per_gather = 9 (the rf-fixed sweep's setting).

At the server default (80) Postgres picks plans where every worker builds its own full aggregate over
lineitem, and both queries hit the 1800 s timeout in backends_20261007_pg/postgres/read.  Same harness
(pgconc run_sql_backend.py read), same seed / parameter sets / reps as that run.
"""
import sys
sys.path.insert(0, '/home/akhilvb/accelerator-codegen/.worktrees/pgconc/benchmarks/compare')
import run_sql_backend as R

orig = R.cmd_read
def cmd_read(a):
    a.pg_workers = 9
    print(f'# read mode with max_parallel_workers_per_gather = {a.pg_workers} for every session', flush=True)
    orig(a)
R.cmd_read = cmd_read
R.main()
