#!/usr/bin/env python3
"""SEC-EDGAR substitution parameters for the Verus kits, from the domains in params/q<N>.json.

Same contract as eval/compare/gen_tpch_params.py (the benchmark pack's `params_generator`):

    python3 eval/bench/sec_edgar/gen_params.py [sets=8] [seed=1]

prints {"seed", "queries": {"<N>": {"sets": [{"params", "verus"}]}}}, where `verus` is the kit's
argument list: SQL literals in the $n order of workloads/sec_edgar/q<N>.sql (the json's `params` order).
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "sweep"))
import sweep_params  # noqa: E402

SETS = int(sys.argv[1]) if len(sys.argv) > 1 else 8
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1


def distinct_sets(spec, n, seed):
    """Up to n distinct draws (a small domain may have fewer)."""
    rng = random.Random(f"{seed}:{spec['query']}")
    sets, seen = [], set()
    for _ in range(n * 20):
        vals = sweep_params._draw_one(spec, rng)
        args = ", ".join(sweep_params.lit(vals[p["name"]]) for p in spec["params"])
        if args not in seen:
            seen.add(args)
            sets.append({"params": vals, "verus": args})
        if len(sets) == n:
            break
    return sets


out = {"seed": SEED, "queries": {}}
for name in sorted(os.listdir(sweep_params.PARAM_DIR)):
    spec = json.load(open(os.path.join(sweep_params.PARAM_DIR, name)))
    if spec["params"]:
        out["queries"][str(spec["query"])] = {"sets": distinct_sets(spec, SETS, SEED)}
json.dump(out, sys.stdout, indent=1)
print()
