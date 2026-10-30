"""SEC-EDGAR parameter draws and baseline text for run_compare.py, in gen_tpch_params.py's shape.

The specs are eval/bench/sec_edgar/params/q<N>.json and the draws sweep_params.py's, so a seed gives the same sets
here and in the Postgres sweeps.  The accelerator and the baseline take the same argument list: the baseline text
is the spec's SQL template with :name replaced by $<position of name>.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "eval", "bench", "sec_edgar", "sweep"))
import sweep_params  # noqa: E402


def draw(query, sets, seed):
    """Up to `sets` distinct parameter sets of the query (one for a query without parameters)."""
    spec = sweep_params.load_spec(query)
    if not sweep_params.has_params(spec):
        drawn = [{}]
    else:
        drawn = []
        for vals in sweep_params.draw_sets(spec, sets * 4, seed):
            if vals not in drawn:
                drawn.append(vals)
            if len(drawn) == sets:
                break
    args = lambda vals: ", ".join(sweep_params.lit(vals[p["name"]]) for p in spec["params"])
    return {"cpp_id": f"sec_edgar:{query}", "note": "",
            "sets": [{"params": vals, "cpp": args(vals), "verus": args(vals)} for vals in drawn]}


def baseline_sql(query):
    spec = sweep_params.load_spec(query)
    sql = spec["sql_template"]
    for i, p in enumerate(spec["params"], 1):
        sql = re.sub(r":%s\b" % re.escape(p["name"]), f"${i}", sql)
    return sql


def write_baselines(directory, queries):
    """<directory>/q<N>.sql for each query; returns the directory."""
    os.makedirs(directory, exist_ok=True)
    for q in queries:
        with open(os.path.join(directory, f"q{q}.sql"), "w") as f:
            f.write(baseline_sql(q) + "\n")
    return directory
