# Clean-tree regeneration

The clean tree is the frozen `substrate/` plus the scaffolding the pipelines need
(`pipeline/`, `workloads/`, `cmake/`, the root CMake, engine-neutral `eval/` and
`scripts/`). The pipelines regenerate everything else: `backends/<engine>/`,
`accelerators/<bench>/`, and `accelerators/prims/` (grown from an empty catalogue).
`manifest.yaml` lists what is kept, generated and dropped. `check_leakage.py` reports
the places where kept files still name generated material.

## 1. Export the tree

```bash
SRC=/flash1/akhilvb/pipe/final                 # the pipe/final worktree (or any checkout of the repo)
DEST=/flash1/akhilvb/regen/tree                # must not exist
"$SRC"/pipeline/clean_tree/make_clean_tree.sh "$SRC" "$DEST" pipe/final
```

The tree is exported with `git archive` into a fresh repository with a single commit.
It has no remote and no link back to `$SRC`.

## 2. Enter the sandbox

```bash
cp $HOME/accelerator-codegen/.env "$DEST/.env"    # OPENROUTER_API_KEY etc.; the pipeline loads $DEST/.env
export ACCEL_DUCKDB_ROOT=$HOME/duckdb-dbcapture
export ACCEL_DUCKDB_SEED_DIRS=$HOME/.cache/accel_pg/build/duckdb:$HOME/.cache/accel_mysql/build/duckdb
export ACCEL_SANDBOX_DATA=/flash1/akhilvb/datasets/tpch_sf1_tbl:/flash1/akhilvb/tpch_sf1.pristine_20261016.duckdb:$HOME/accelerator-codegen-duckdb
"$DEST"/pipeline/clean_tree/sandbox.sh "$DEST"
```

Inside the sandbox:

- the tree is read-write;
- the data, DuckDB and seed paths are read-only (the `.env` dataset paths open read-only too;
  step 3 points `ACCEL_TPCH_DB` at a writable copy, and exported values win over `.env`);
- `/flash1/$USER/engine_state` is read-write;
- the Docker socket is available;
- the rest of `/home` and `/flash1` is hidden.

The Docker daemon runs outside the sandbox and can mount any host path. Agents only
reach Docker through `backends/<engine>/host.sh`, under the pipeline's allowed-command
lists.

## 3. Inside the sandbox

```bash
cd "$ACCEL_TREE"                                   # the tree root (sandbox.sh exports it)
python3 -m venv .venv && .venv/bin/pip install -q -r pipeline/orchestrator/requirements.txt
export PY="$PWD/.venv/bin/python" PYTHONPATH="$PWD/pipeline/orchestrator"
export ACCEL_DATASET_DIR=/flash1/akhilvb/datasets/tpch_sf1_tbl
export ACCEL_TPCH_DB=$PWD/.data/tpch_sf1.duckdb
mkdir -p .data && cp /flash1/akhilvb/tpch_sf1.pristine_20261016.duckdb "$ACCEL_TPCH_DB"
# Optional, faster container builds: a prebuilt in-container DuckDB for the engine being built
# (~/.cache/accel_pg/build/duckdb for postgres, ~/.cache/accel_mysql/... for mysql; bind them via ACCEL_DUCKDB_SEED_DIRS).
# export ACCEL_DUCKDB_SEED=...

# Engine backends, one per engine (each writes backends/<engine>/, host.sh first),
# then the accelerators (22 queries x 3 engines, each base accelerator first; the first
# wave, Q1 on every engine, grows prims/ before the rest fan out). Stops at the first failure.
regen() {
  local e
  for e in duckdb postgres mysql; do
    $PY -m accel_pipeline.run_pipeline --workload engine_$e --query 0 --stage all || return
  done
  $PY -m accel_pipeline.run_pack --pack tpch_regen_all22
}
regen
```

Add `--dry-run` to either command to render the prompts and gate contexts without
calling agents.

## Checks while it runs

```bash
python3 pipeline/clean_tree/check_leakage.py --markdown /tmp/leakage.md   # kept files naming generated material
git -C "$DEST" log --oneline | head                                        # what the agents committed
```
