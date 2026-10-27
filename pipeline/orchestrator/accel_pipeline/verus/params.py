"""Random query parameters for the Verus pipeline's measurements and tests.

Every substitution parameter of a query is a runtime parameter of its kit, so
every benchmark, profile and correctness run draws fresh values from the
parameter domain (TPC-H spec 2.4 / qgen ranges): a kernel specialized to fixed
parameter values gains nothing.  The domain lives in
``eval/compare/gen_tpch_params.py`` (all 22 queries; its ``verus`` field
is the kit's argument list in $n order); this module only calls it with a seed.

A *draw* is one seed and the parameter tuples it gives, written to the run's
artifacts (``<art>/param_draws/<tag>_<seed>.json``).  Nothing is written before a
measurement starts, so the agent cannot read the values ahead of it.  Paired
comparisons take one draw and hand it to both sides.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.test_suite import fresh_seed, n_params, split_tuple
from accel_pipeline.verus.context import VerusContext

GENERATOR = "eval/compare/gen_tpch_params.py"


@dataclass
class Draw:
    tag: str
    seed: int
    sets: list[str]  # SQL literal tuples, one per parameter set, in $n order
    path: Path  # the tuples, one per line (what the runner reads)
    record: Path  # JSON copy in the run's artifacts
    problems: list[str] = field(default_factory=list)


def _generate(ctx: VerusContext, sets: int, seed: int) -> list[str]:
    """The domain's tuples for this query from the shared generator (run as a script, seeded)."""
    script = Path(str(ctx.setting("params.generator") or ctx.root / GENERATOR))
    if ctx.benchmark != "tpch" or not script.is_file():
        return []
    proc = subprocess.run([sys.executable, str(script), str(sets), str(seed)], capture_output=True, text=True,
                          timeout=120, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"parameter generator failed: {proc.stderr[-500:]}")
    q = json.loads(proc.stdout)["queries"].get(str(ctx.query)) or {}
    return [s["verus"] for s in q.get("sets") or [] if s.get("verus")]


def new_draw(ctx: VerusContext, tag: str, *, sets: int | None = None, seed: int | None = None) -> Draw | None:
    """A fresh random draw of parameter tuples from the query's domain (None: the query has no
    parameters, or the benchmark has no generator -- the frozen tuples are used).

    `seed` is for replaying a recorded draw; a run never sets it (setting `params.seed` pins
    it, for tests only)."""
    want = n_params(Path(ctx.query_sql).read_text(encoding="utf-8")) if ctx.query_sql and Path(ctx.query_sql).is_file() else 0
    if want == 0:
        return None
    if seed is None:
        seed = ctx.setting("params.seed")
    seed = int(seed) if seed is not None else fresh_seed()
    count = int(sets if sets is not None else ctx.setting("params.sets", 5))
    tuples = _generate(ctx, count, seed)
    problems = []
    good = []
    for t in tuples:
        if len(split_tuple(t)) == want:
            good.append(t)
        else:
            problems.append(f"{t!r}: {len(split_tuple(t))} values, the query takes {want}")
    if not good:
        raise RuntimeError(f"no parameter domain for {ctx.benchmark} q{ctx.query} ({want} parameters): "
                           + ("; ".join(problems[:2]) or "the generator gives none")
                           + ". Every parameter must be drawn from its domain (pipeline/verus/params.py)")
    stamp = f"{tag}_{seed}"
    path = Path(ctx.run_dir) / "param_draws" / f"{stamp}.params"
    record = ctx.art / "param_draws" / f"{stamp}.json"
    for p in (path, record):
        p.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(good) + "\n", encoding="utf-8")
    record.write_text(json.dumps({"tag": tag, "seed": seed, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                  "query": ctx.query, "sets": good, "problems": problems}, indent=2),
                      encoding="utf-8")
    return Draw(tag=tag, seed=seed, sets=good, path=path, record=record, problems=problems)


def runner_env(draw: Draw | None, mode: str) -> dict[str, str]:
    """Environment for verus_kit_runner: `replace` (bench, profile, snapshots: these tuples are the
    parameter sets) or `extra` (differential: these tuples are checked besides the frozen ones)."""
    if draw is None:
        return {}
    return {"VERUS_KIT_PARAMS_FILE" if mode == "replace" else "VERUS_KIT_EXTRA_PARAMS_FILE": str(draw.path)}


def summary(draw: Draw | None) -> dict[str, Any]:
    """What a result records about its draw."""
    return {} if draw is None else {"param_seed": draw.seed, "param_sets": draw.sets, "param_record": str(draw.record)}


def check_query(ctx: VerusContext) -> str:
    """'' if every substitution parameter of the query is a $n parameter; else what is wrong (preflight).
    A substitution parameter left as a literal (a LIKE pattern, a factor) would let a kernel specialize to
    one value: the query then takes fewer $n than its domain has.  Literals the benchmark itself fixes
    (TPC-H Q16's '%Customer%Complaints%') are not substitution parameters and are fine."""
    if not ctx.query_sql or not Path(ctx.query_sql).is_file():
        return ""
    sql = Path(ctx.query_sql).read_text(encoding="utf-8")
    if ctx.benchmark != "tpch":
        return ""
    want = n_params(sql)
    gen = _generate(ctx, 1, 1)
    if want and not gen:
        return (f"no parameter domain for tpch q{ctx.query} in {GENERATOR} (its `verus` argument list is empty): "
                "every substitution parameter must be drawn from its domain")
    if len(split_tuple(gen[0])) != want:
        return (f"{ctx.query_sql} takes {want} $n parameters but the TPC-H domain of q{ctx.query} has "
                f"{len(split_tuple(gen[0]))} ({gen[0]}): a substitution parameter is a literal (or an extra one is declared)")
    return ""
