#!/usr/bin/env python3
"""Random-parameter draws for the SEC-EDGAR sweeps, driven by ../params/q<N>.json.

Both engines use the same seeded sequence: draw i of query N depends only on
(seed, N), so DuckDB and Postgres, base and shared, see identical parameters.

Library: load_spec, draw_sets, render_base, render_call.
CLI (used by the DuckDB benchmark): writes a file with one line per draw,
"base_sql<TAB>call_sql", the read draws first, then a "#check" line and the
correctness-check draws.

  sweep_params.py --query 2 --reads 8 --checks 4 --seed 1 --out draws.tsv
"""
import argparse
import json
import os
import random
import re

PARAM_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "params")


def load_spec(query):
    with open(os.path.join(PARAM_DIR, f"q{query}.json")) as f:
        return json.load(f)


def _draw_one(spec, rng):
    vals = {}
    for p in spec["params"]:
        if p["name"] in vals:
            continue
        dom = p["domain"]
        if isinstance(dom, str) and dom.startswith("joint:"):
            names = dom[len("joint:"):].split(",")
            choice = rng.choice(spec["joint_domains"][dom[len("joint:"):]])
            vals.update(zip(names, choice))
        else:
            vals[p["name"]] = rng.choice(dom)
    return vals


def draw_sets(spec, n, seed, skip=0):
    """n parameter dicts, after skipping `skip` draws of the same stream."""
    rng = random.Random(f"{seed}:{spec['query']}")
    out = [_draw_one(spec, rng) for _ in range(skip + n)]
    return out[skip:]


def defaults(spec):
    return {p["name"]: p["default"] for p in spec["params"]}


def lit(v):
    return "'" + v.replace("'", "''") + "'" if isinstance(v, str) else str(int(v))


def render_base(spec, vals):
    # longest names first so :sic_lo is not clobbered by a shorter prefix
    sql = spec["sql_template"]
    for name in sorted(vals, key=len, reverse=True):
        sql = re.sub(r":%s\b" % re.escape(name), lit(vals[name]), sql)
    return sql


def render_call(spec, vals):
    args = ", ".join(lit(vals[p["name"]]) for p in spec["params"])
    return f"SELECT * FROM {spec['function']}({args})"


def has_params(spec):
    return bool(spec["params"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", type=int, required=True)
    ap.add_argument("--reads", type=int, required=True)
    ap.add_argument("--checks", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    spec = load_spec(a.query)
    if not has_params(spec):  # q1: every draw is the zero-argument query
        sets = [{}] * (a.reads + a.checks)
    else:
        sets = draw_sets(spec, a.reads + a.checks, a.seed)
    one = lambda s: " ".join(render_base(spec, s).split()) + "\t" + (
        render_call(spec, s) if has_params(spec) else f"SELECT * FROM {spec['function']}()")
    with open(a.out, "w") as f:
        for s in sets[:a.reads]:
            f.write(one(s) + "\n")
        f.write("#check\n")
        for s in sets[a.reads:]:
            f.write(one(s) + "\n")


if __name__ == "__main__":
    main()
