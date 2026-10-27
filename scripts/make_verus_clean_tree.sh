#!/usr/bin/env bash
# Make a clean tree for a from-scratch Verus pipeline run: this checkout's
# HEAD exported WITHOUT git history (a fresh one-commit repo), minus every
# accelerator (per-query C++ trees, hand-written Verus kernels and their
# tests/specs, pipeline artifacts).  What stays: the substrate, the runtime
# backends, substrate/verus/{common,store,kit,sql2verus} and accelerators/prooflib
# (prooflib and the playbook as seeded), the query SQL, and the pipeline.
#
# Also sets up what is gitignored: .env, the pipeline's Codex home (config and
# MCP launcher only, no memories; paths rewritten), the orchestrator venv.
# The Verus binary is used in place (VERUS_BIN; the sandbox mounts it).
#
# --no-backends: also drop every engine backend (backends/*), for the clean
# backend regeneration (docs/codebase-layout-proposal.md): the engine_backend
# pipeline then regenerates them from the substrate. pipeline/artifacts (incl.
# engines/ and backend_additions/) is dropped either way. accelerators/prims,
# accelerators/prooflib and pipeline/verus/playbook are kept as they are at HEAD
# (no seed snapshot is defined yet; reset them by hand if the run needs it).
#
# usage: scripts/make_verus_clean_tree.sh [--no-backends] <dest>   (dest must not exist)
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NO_BACKENDS=0
if [[ "${1:-}" == "--no-backends" ]]; then NO_BACKENDS=1; shift; fi
DEST="${1:?usage: make_verus_clean_tree.sh [--no-backends] <dest>}"
[[ -e "${DEST}" ]] && { echo "${DEST} exists" >&2; exit 1; }
DEST="$(mkdir -p "${DEST}" && cd "${DEST}" && pwd)"

git -C "${SRC}" archive HEAD | tar -x -C "${DEST}"
cd "${DEST}"
shopt -s nullglob
# a benchmark's build.cmake and Verus world name the queries removed here (the pipeline regenerates both)
remove=(
  accelerators/*/q[0-9]*
  accelerators/*/build.cmake accelerators/*/verus_world
  workloads/*/q[0-9]*.inputs
  pipeline/artifacts pipeline/verus_runs
)
(( NO_BACKENDS )) && remove+=(backends/*)
for p in "${remove[@]}"; do rm -rf -- "${DEST:?}/${p}"; done
mkdir -p pipeline/artifacts
# tracked symlinks into untracked build dirs (e.g. build-release -> build) dangle here
for l in build*; do
  if [[ -L "${l}" && ! -e "${l}" ]]; then rm -f -- "${l}"; mkdir -p "${l}"; fi
done

git init -q && git add -A && git -c user.name="clean tree" -c user.email="clean@localhost" \
  commit -q -m "Clean tree: substrate + runtime backends + Verus seeds (exported from $(git -C "${SRC}" rev-parse --short HEAD))"

[[ -f "${SRC}/.env" ]] && cp "${SRC}/.env" .env
# the Verus binary, used in place (the sandbox mounts it): the pipeline reads VERUS_BIN from .env
VERUS="${VERUS_BIN:-${SRC}/tools/verus/verus-x86-linux/verus}"
[[ -x "${VERUS}" ]] || { echo "no Verus binary at ${VERUS} (set VERUS_BIN)" >&2; exit 1; }
grep -q '^VERUS_BIN=' .env 2>/dev/null || echo "VERUS_BIN=${VERUS}" >> .env
mkdir -p pipeline/.codex
for f in config.toml run_mcp_server.sh; do
  [[ -f "${SRC}/pipeline/.codex/${f}" ]] && cp "${SRC}/pipeline/.codex/${f}" "pipeline/.codex/${f}"
done
[[ -f pipeline/.codex/config.toml ]] && sed -i "s|${SRC}|${DEST}|g" pipeline/.codex/config.toml

python3 -m venv pipeline/orchestrator/.venv
pipeline/orchestrator/.venv/bin/pip install -q -e pipeline/orchestrator

cat <<EOF
clean tree: ${DEST}  ($(git log --oneline | wc -l) commit, no history)
removed: ${remove[*]}

run (agents are sandboxed to this tree by bwrap; preflight checks it):
  export VERUS_BIN=${VERUS}
  cd ${DEST}/pipeline/orchestrator && .venv/bin/python -m accel_pipeline.run_pipeline \\
    --query 21 --workload tpch_q21_aux --mode verus --models-config pipeline/config/models.codex.yaml
EOF
