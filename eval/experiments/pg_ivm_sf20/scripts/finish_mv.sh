#!/usr/bin/env bash
# Paper run: burton, 2026-10-19/20. Output $R/mvcmp is data/ here: accel_hi.console.log, accel_settings_hi.txt,
# mv.console.log, final_design.txt, final_views.txt and the rf-fixed sweep under postgres/ (inputs of ../plot.py).
# Finish the accel-vs-views comparison after the 40v40 read sweep (tmux pg40):
#   1. back to the all-core setup of the Oct 19 accel sweep (CPUs 0-79, PG 80 workers, accel 80 / read 10)
#   2. rerun the accel points that died on "update set 41 missing" (1000 / 5000 rows/s, uncapped); the stream now wraps
#      to the next 500-order slice of each set (the first 40 RF pairs are unchanged, so the clean 0-200 points stand),
#      and accel.commit_ship_timeout_ms = 600000 so an overloaded worker throttles the writer instead of rebuilding
#   3. build every view (v1 all 22 + v2 candidates of Q2 Q3 Q10 Q15), check all against plain Postgres
#   4. keep a v2 design only if it matches everywhere and reads >= 2x faster (median over the draws + edges); its write
#      cost passed in phase 1 (mv/maint/summary.json: <= 0.3 s per slot); drop the losers (every view present is maintained)
#   5. the MV sweep: same rates, seed, readers, windows as the accel sweep; then drop every view
set -uo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
cd "$REPO"
R=${R:?set R to the work root holding mv/ and mvcmp/ (paper run: pg_sf20r)}
export RF_DIR=${RF_DIR:?set RF_DIR to the dbgen refresh sets (rf1/rf2 update files) for SF20}
M=$R/mv; O=$R/mvcmp; T=accel_pg_sf20r
export PATH=$REPO/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460 PG_DB=accel_tpch_sf20
PSQL="psql -h 127.0.0.1 -p 5460 -U postgres -d accel_tpch_sf20 -Atq"
SEED=20261018; V2Q="2 3 10 15"
COMMON_ENV=(PYTHON=python3 BACKENDS=postgres SF=20 READERS=8 PG_WORKERS=9 REPEATS=3 SEED=$SEED BATCH_ORDERS=500
            "RF_EXTRA=--max-commits 15000" QUERY_TIMEOUT=1800 FIXED_WARMUP=30
            MIN_READS=3 MAX_WINDOW=900)
ivm() { $PSQL -c "select count(*) from pg_trigger where tgname like 'IVM_trigger%'"; }

echo "=== $(date) waiting for pg40 (run_40.log ALL DONE)"
until grep -q '=== .* ALL DONE' $R/run_40.log 2>/dev/null; do
  tmux has-session -t pg40 2>/dev/null || { echo "$(date) pg40 ended without ALL DONE - stopping"; exit 1; }
  sleep 60
done
while pgrep -f 'scripts/pg_base_bench.py|scripts/run_base_postgres.sh|run_sweep.sh' >/dev/null; do sleep 10; done

echo "=== $(date) all-core setup"
docker update --cpuset-cpus 0-79 $T >/dev/null
$PSQL -c "ALTER SYSTEM SET max_parallel_workers = 80" -c "ALTER SYSTEM SET max_parallel_workers_per_gather = 80" \
      -c "ALTER SYSTEM SET accel.read_threads = 10" -c "ALTER SYSTEM SET accel.maint_threads = 80" \
      -c "ALTER SYSTEM SET accel.build_threads = 80" -c "ALTER SYSTEM SET accel.threads = 80" \
      -c "ALTER SYSTEM SET accel.shard_read_threads = 64" -c "ALTER SYSTEM SET accel.commit_ship_timeout_ms = 600000"
docker restart $T >/dev/null; until $PSQL -c 'select 1' >/dev/null 2>&1; do sleep 1; done
for t in region nation customer supplier part partsupp orders lineitem; do $PSQL -c "ALTER TABLE $t RESET (parallel_workers)"; done
$PSQL -c "LOAD 'accelerator_codegen_pg'" -c "select string_agg(name||'='||setting, ' ' order by name) from pg_settings where name like 'accel.%' or name in ('max_parallel_workers','max_parallel_workers_per_gather','max_worker_processes','jit')" \
  | tee $O/accel_settings_hi.txt
echo "cpuset $(docker inspect -f '{{.HostConfig.CpusetCpus}}' $T)  IVM triggers $(ivm) (must be 0)"
[ "$(ivm)" = 0 ] || { echo "IVM triggers present - stopping"; exit 1; }

echo "=== $(date) accel rerun: 1000 / 5000 rows/s + uncapped (the old runs -> $O/invalid_set41)"
mkdir -p $O/invalid_set41
for iv in 2.686 0.537 uncapped; do
  [ -d $O/postgres/rf-fixed/interval${iv}s ] && mv $O/postgres/rf-fixed/interval${iv}s $O/invalid_set41/
done
env "${COMMON_ENV[@]}" ONLY_PHASES=accel FIXED_WINDOW=300 FIXED_INTERVALS="2.686 0.537 uncapped" \
  bash eval/compare/run_backends.sh rf-fixed $O 2>&1 | tee -ia $O/accel_hi.console.log
echo "=== $(date) accel rerun done: $(grep -c 'RUN INVALID' $O/accel_hi.console.log) invalid, $(grep -c 'WRITER STOPPED' $O/accel_hi.console.log) writer stops"

echo "=== $(date) views: stop shard workers, build v1 (22) + v2 ($V2Q)"
$PSQL -c "SELECT accel_postgres_stop_shard_workers()" >/dev/null 2>&1
MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py create --schema public --jobs 8 2>&1 | tee $M/final_build_v1.log || exit 1
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py create --schema public --jobs 4 --queries "$V2Q" 2>&1 | tee $M/final_build_v2.log || exit 1

echo "=== $(date) check v1 (8 draws + edges, 9 workers like a reader)"
MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py check --schema public --sets 8 --edges --seed $SEED --workers 9 --out $M/check_draws 2>&1 | tee $M/final_check_v1.log
echo "=== $(date) check v2"
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py check --schema public --sets 8 --edges --seed $SEED --workers 9 --queries "$V2Q" --out $M/check_draws 2>&1 | tee $M/final_check_v2.log
grep -q '^mismatches: 0$' $M/final_check_v1.log || { echo "v1 CHECK FAILED - stopping with the views in place"; exit 1; }

echo "=== $(date) pick designs"
python3 -B - $M <<'PY' || exit 1
import re, statistics, sys, os
sys.path.insert(0, "eval/compare")
m = sys.argv[1]
def parse(f):
    out = {}
    for l in open(f):
        r = re.match(r"Q(\d+)\s+(ok|DIFF).* mv\s+([\d.]+) ms", l)
        if r:
            q = int(r.group(1)); out.setdefault(q, []).append((r.group(2) == "ok", float(r.group(3))))
    return out
v1, v2 = parse(f"{m}/final_check_v1.log"), parse(f"{m}/final_check_v2.log")
pick = []
for q in (2, 3, 10, 15):
    a, b = statistics.median(t for _, t in v1[q]), statistics.median(t for _, t in v2.get(q, [(0, 1e18)]))
    ok = q in v2 and all(o for o, _ in v2[q])
    use = ok and a / b >= 2
    print(f"Q{q}: v1 median {a:.2f} ms, v2 {b:.2f} ms ({a / b:.1f}x), v2 check {'ok' if ok else 'FAILED'} -> {'v2' if use else 'v1'}")
    if use: pick.append(q)
open(f"{m}/final_design.txt", "w").write(" ".join(map(str, pick)) + "\n")
PY
DESIGN=$(cat $M/final_design.txt); echo "MV_DESIGN=\"$DESIGN\""
LOSERS=$(MV_DESIGN="$DESIGN" python3 -B - "$V2Q" <<'PY'
import os, sys
sys.path.insert(0, "eval/compare")
import mv_defs as D
kept = {n for q in D.IMMV for n in D.IMMV[q]}
import importlib
allnames = set()
for d in ("v1", "v2"):
    os.environ["MV_DESIGN"] = d; importlib.reload(D)
    allnames |= {n for q in map(int, sys.argv[1].split()) for n in D.IMMV[q]}
print(" ".join(sorted(allnames - kept)))
PY
)
echo "dropping unused designs: $LOSERS"
for n in $LOSERS; do $PSQL -c "DROP TABLE IF EXISTS public.$n CASCADE"; done
$PSQL -c "select string_agg(relname, ' ' order by relname) from pg_class where relname like 'mv\_%' and relkind in ('r','m')" | tee $M/final_views.txt
echo "IVM triggers: $(ivm)"

echo "=== $(date) MV sweep (MV_DESIGN=\"$DESIGN\", refresh concurrent)"
env "${COMMON_ENV[@]}" ONLY_PHASES=mv MV_SCHEMA=public MV_CHUNK=500 MV_DESIGN="$DESIGN" FIXED_WINDOW=300 \
  FIXED_INTERVALS="0 53.7 13.43 2.686 0.537 uncapped" bash eval/compare/run_backends.sh rf-fixed $O 2>&1 | tee -ia $O/mv.console.log
env "${COMMON_ENV[@]}" ONLY_PHASES=mv MV_SCHEMA=public MV_CHUNK=500 MV_DESIGN="$DESIGN" FIXED_WINDOW=900 \
  FIXED_INTERVALS="268.6" bash eval/compare/run_backends.sh rf-fixed $O 2>&1 | tee -ia $O/mv.console.log

echo "=== $(date) drop every view"
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py drop --schema public; MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py drop --schema public
echo "IVM triggers left: $(ivm)"
echo "=== $(date) MV SWEEP DONE"
