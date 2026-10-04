#!/usr/bin/env bash
# Compile-check one source file of accelerator_codegen with its real flags
# (from build-verus-duckdb/compile_commands.json) into a scratch object,
# without touching the build tree (safe to run in parallel).
#   scripts/compile_one.sh src/generated/tpch/q3/duckdb/aux/module.cpp
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FILE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
python3 - "$ROOT" "$FILE" <<'PY'
import json, shlex, subprocess, sys, os, tempfile
root, f = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(root, "build-verus-duckdb/compile_commands.json")))
e = next((x for x in cc if os.path.realpath(x["file"]) == os.path.realpath(f)), None)
like = None
if e is None:  # a new file: borrow a benchmark's flags
    like = os.path.realpath(os.path.join(root, "benchmarks/aux_state/calibrate_shared_aux_cost_model.cpp"))
    e = next((x for x in cc if os.path.realpath(x["file"]) == like), None)
    if e is None:
        sys.exit(f"{f}: not in compile_commands.json")
args = shlex.split(e["command"])
if like:
    args = [f if os.path.realpath(os.path.join(e["directory"], a)) == like else a for a in args]
out = tempfile.mktemp(suffix=".o", dir=os.environ.get("TMPDIR", "/tmp"))
i = args.index("-o"); args[i + 1] = out
args = [a for a in args if not a.startswith("-MD") and a not in ("-MF",)]
r = subprocess.run(args, cwd=e["directory"])
if os.path.exists(out): os.remove(out)
sys.exit(r.returncode)
PY
