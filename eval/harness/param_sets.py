"""The parameter sets a Python test or bench runs a query with: the counterpart of param_source.hpp.

Source: the JSON file $ACCEL_PARAM_FILE if set, else
  python3 <repo>/eval/bench/<workload>/gen_params.py $ACCEL_PARAM_SETS $ACCEL_PARAM_SEED
(sets default 8, seed default a fresh random one). Prints "[params] <workload> q<N> seed=<s> sets=<k>"
to stderr; reproduce a run with ACCEL_PARAM_SEED=<s>.
"""
import json
import os
import random
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def draw_sets(workload, query_id):
    """The generator's sets for the query: dicts {"params": {name: value}, "cpp": "<SQL literal list>"}."""
    path = os.environ.get("ACCEL_PARAM_FILE")
    if path:
        with open(path) as f:
            doc = json.load(f)
    else:
        sets = os.environ.get("ACCEL_PARAM_SETS", "8")
        seed = os.environ.get("ACCEL_PARAM_SEED") or str(random.randrange(10**9))
        script = os.path.join(os.environ.get("ACCEL_SRC_DIR", REPO), "eval", "bench", workload, "gen_params.py")
        doc = json.loads(subprocess.check_output([sys.executable, script, sets, seed]))
    drawn = doc["queries"][str(query_id)]["sets"]
    print(f"[params] {workload} q{query_id} seed={doc['seed']} sets={len(drawn)}", file=sys.stderr)
    return drawn
