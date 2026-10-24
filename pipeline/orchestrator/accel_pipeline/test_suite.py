"""The per-query test suite: its declarative format, mechanical validation, random draws, and freezing.

One suite per query, in ``layout.suite_dir`` (the format the Verus pipeline's tests
stage writes too; the engine harness runs it):

    params.sql    fixed parameter tuples, one per line, as SQL literals in the query's $n order
    domain.yaml   the parameter domain fresh random tuples are drawn from on every run
    txns/NNN.sql  write transactions, applied in order, committed (a first line ``-- rollback``: applied, then aborted)
    write_mix.yaml  the write-frequency range benchmarks sample from, and the write kinds' relative weights
    suite.yaml    manifest: what each transaction is, edge-case coverage, scale scenarios, bulk and point minimums
    setup.sql     optional; shapes the data before the accelerator is built

The test_suite stage writes it and validates it; afterwards it is frozen (content hash in the artifacts)
and the gates verify the hash before they run.  A genuinely wrong test is raised as a change request, never edited.
"""

from __future__ import annotations

import calendar
import datetime as dt
import hashlib
import json
import random
import re
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.layout import suite_dir
from accel_pipeline.sql_tuples import fresh_seed, n_params, split_tuple

PARAMS, DOMAIN, WRITE_MIX, MANIFEST, TXNS = "params.sql", "domain.yaml", "write_mix.yaml", "suite.yaml", "txns"
FROZEN_RECORD = "test_suite_frozen.json"
CHANGE_REQUEST = "test_suite_change_request.md"
ROLLBACK_MARK = "-- rollback"

EDGE_CLASSES = ("in_spec", "off_spec", "null", "empty_result", "limit_ties", "extreme_values", "large_keys")
TXN_KINDS = ("point_insert", "point_update", "point_delete", "bulk_insert", "bulk_update", "bulk_delete",
             "sequence", "rollback", "key_move", "other")
VERBS = ("INSERT", "UPDATE", "DELETE")
DOMAIN_TYPES = ("choice", "int", "decimal", "date", "pattern", "derived")


@dataclass(frozen=True)
class SuiteConfig:
    """``test_suite:`` of config/default.yaml."""

    acceptance_random_tuples: int = 20
    quick_random_tuples: int = 4
    quick_txns: int = 4
    differential_every: int = 5
    validation_random_tuples: int = 12
    bench_repeats: int = 3
    write_mix: dict[str, Any] | None = None

    @classmethod
    def from_dict(cls, raw: dict[str, Any] | None) -> SuiteConfig:
        raw = dict(raw or {})
        known = {k: int(raw[k]) for k in cls.__dataclass_fields__ if k != "write_mix" and k in raw}
        return cls(**known, write_mix=dict(raw.get("write_mix") or {}) or None)


def suite_path(repo_root: Path, benchmark: str, query: int) -> Path:
    return repo_root / suite_dir(benchmark, query)


def gate_settings(pipeline_config: Any, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """What the gates need to run the query's suite (the gate context's `test_suite`)."""
    from accel_pipeline.layout import workload_query_sql, workload_schema_constraints
    from accel_pipeline.naming import aux_function_name, base_function_name

    root, bench = pipeline_config.repo_root, str(workload.get("benchmark_name", "tpch"))
    aux = workload.get("workload_kind") == "aux_state"
    constraints = workload_schema_constraints(bench)
    return {
        "dir": str(suite_path(root, bench, query)),
        "query_sql": str(root / workload_query_sql(bench, query)),
        "function": (aux_function_name if aux else base_function_name)(bench, query),
        "arity": len(workload.get("parameters") or []),
        "param_names": [str(p["name"]) for p in workload.get("parameters") or []],
        "aux": aux,
        "schema_sql": str(root / constraints) if (root / constraints).is_file() else "",
        "config": asdict(pipeline_config.test_suite),
    }


# ---------------------------------------------------------------- reading


def _yaml(path: Path) -> dict[str, Any]:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f"{path.name}: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{path.name} must be a mapping")
    return data


def dependent_tables(art: Path) -> list[str]:
    """The tables whose writes can change the query's answer, from the analysis."""
    try:
        facts = json.loads((art / "analysis.json").read_text(encoding="utf-8")).get("correctness_facts") or {}
    except (OSError, ValueError):
        return []
    return [str(t["name"]) for t in facts.get("dependent_tables") or [] if isinstance(t, dict) and t.get("name")]


def read_write_mix(suite: Path) -> dict[str, Any]:
    return _yaml(suite / WRITE_MIX)


def read_params(path: Path) -> list[str]:
    """The tuples of a params file: one per non-empty, non-comment line."""
    if not path.is_file():
        return []
    return [ln.strip() for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.lstrip().startswith("--")]


def txn_files(directory: Path) -> dict[str, str]:
    """{file name: SQL} of the write transactions, in the order they run."""
    return {f.name: f.read_text(encoding="utf-8") for f in sorted((directory / TXNS).glob("*.sql"))}


def statements(sql: str) -> list[tuple[str, str]]:
    """(verb, table) of each INSERT / UPDATE / DELETE statement of a transaction."""
    text = re.sub(r"--[^\n]*", "", sql)
    found = re.findall(r"(?:^|;)\s*(INSERT\s+INTO|UPDATE|DELETE\s+FROM)\s+([A-Za-z_]\w*)", text, re.I)
    return [(v.split()[0].upper(), t.lower()) for v, t in found]


def is_rollback(sql: str) -> bool:
    return sql.lstrip().lower().startswith(ROLLBACK_MARK)


# ---------------------------------------------------------------- random draws (reproducible from a seed)


def _add_months(day: dt.date, months: int) -> dt.date:
    index = day.month - 1 + months
    year, month = day.year + index // 12, index % 12 + 1
    return dt.date(year, month, min(day.day, calendar.monthrange(year, month)[1]))


def _one(spec: dict[str, Any], rng: random.Random, drawn: dict[str, Any]) -> tuple[str, Any]:
    """(SQL literal, python value) of one parameter."""
    kind = spec["type"]
    if kind == "choice":
        return rng.choice(spec["values"]), None
    if kind == "int":
        value = rng.randint(int(spec["min"]), int(spec["max"]))
        return str(value), value
    if kind == "decimal":
        scale = int(spec.get("scale", 2))
        value = round(rng.uniform(float(spec["min"]), float(spec["max"])), scale)
        return f"{value:.{scale}f}", value
    if kind == "date":
        lo, hi = dt.date.fromisoformat(str(spec["min"])), dt.date.fromisoformat(str(spec["max"]))
        value = lo + dt.timedelta(days=rng.randint(0, (hi - lo).days))
        return f"DATE '{value.isoformat()}'", value
    if kind == "pattern":
        return "'" + str(spec["template"]).replace("{}", rng.choice(spec["words"])) + "'", None
    source = drawn[spec["from"]]
    value = _add_months(source, int(spec["add_months"]))
    return f"DATE '{value.isoformat()}'", value


def draw_tuples(domain: dict[str, Any], count: int, seed: int) -> list[str]:
    """`count` parameter tuples (SQL literals in $n order) from the domain; the same seed gives the same tuples."""
    rng = random.Random(seed)
    tuples = []
    for _ in range(count):
        drawn: dict[str, Any] = {}
        literals = []
        for spec in domain["params"]:
            literal, value = _one(spec, rng, drawn)
            drawn[spec["name"]] = value
            literals.append(literal)
        tuples.append(", ".join(literals))
    return tuples


def draw_write_frequencies(mix: dict[str, Any], count: int, seed: int) -> list[int]:
    """Write frequencies (% of operations) for one benchmark run: 0, then `count` distinct draws from the range
    (log-uniform or uniform), sorted; the same seed gives the same draw."""
    rng = random.Random(seed)
    spec = mix["frequency_pct"]
    lo, hi = int(spec["min"]), int(spec["max"])
    log = spec.get("distribution", "log") == "log"
    drawn: set[int] = set()
    for _ in range(count * 20):
        if len(drawn) >= min(count, hi - lo + 1):
            break
        drawn.add(round(lo * (hi / lo) ** rng.random()) if log else rng.randint(lo, hi))
    return [0, *sorted(drawn)]


# ---------------------------------------------------------------- validation


def _check_domain(domain: dict[str, Any], names: list[str], want: int) -> list[str]:
    params = domain.get("params")
    if not isinstance(params, list) or len(params) != want:
        return [f"{DOMAIN} needs one `params` entry per query parameter ({want}), in $n order"]
    problems = []
    for i, spec in enumerate(params, 1):
        kind = spec.get("type") if isinstance(spec, dict) else None
        if kind not in DOMAIN_TYPES or not spec.get("name"):
            problems.append(f"{DOMAIN} param {i}: needs a name and a type in {DOMAIN_TYPES}")
            continue
        needed = {"choice": ("values",), "int": ("min", "max"), "decimal": ("min", "max"), "date": ("min", "max"),
                  "pattern": ("template", "words"), "derived": ("from", "add_months")}[kind]
        problems += [f"{DOMAIN} param {spec['name']}: `{key}` missing" for key in needed if key not in spec]
    declared = [p.get("name") for p in params if isinstance(p, dict)]
    if names and len(names) == want and declared != names:
        problems.append(f"{DOMAIN} parameter names {declared} must be the workload's {names}")
    return problems


def _check_write_mix(mix: dict[str, Any], txn_kinds: set[str]) -> list[str]:
    spec = mix.get("frequency_pct")
    if not isinstance(spec, dict) or not all(isinstance(spec.get(k), (int, float)) for k in ("min", "max", "samples")):
        return [f"{WRITE_MIX} needs frequency_pct: {{min, max, samples, distribution}}"]
    if not 0 < spec["min"] <= spec["max"] <= 100 or spec["samples"] < 1:
        return [f"{WRITE_MIX} frequency_pct needs 0 < min <= max <= 100 and samples >= 1"]
    kinds = mix.get("kinds")
    if not isinstance(kinds, dict) or not kinds or not all(isinstance(w, (int, float)) and w > 0 for w in kinds.values()):
        return [f"{WRITE_MIX} needs `kinds`: relative weights (> 0) of the write kinds"]
    unknown = sorted(set(kinds) - txn_kinds)
    return [f"{WRITE_MIX} weights kinds no transaction has: {unknown}"] if unknown else []


def _check_edges(manifest: dict[str, Any], tuples: int) -> list[str]:
    edges = manifest.get("edge_coverage")
    if not isinstance(edges, dict):
        return [f"{MANIFEST} needs edge_coverage: {{class: [params.sql tuple numbers] | 'n/a: reason'}}"]
    problems = []
    for cls in EDGE_CLASSES:
        value = edges.get(cls)
        if isinstance(value, str):
            if not value.lower().startswith("n/a:") or len(value) < 12:
                problems.append(f"edge_coverage.{cls}: a string must be 'n/a: <why this class cannot apply>'")
        elif not isinstance(value, list) or not value or not all(isinstance(i, int) and 1 <= i <= tuples for i in value):
            problems.append(f"edge_coverage.{cls}: params.sql tuple numbers, 1-based, comments not counted (1..{tuples}), or 'n/a: reason'")
    return problems


def validate_static(suite: Path, *, query_sql: str, param_names: list[str], dependent_tables: list[str],
                    aux: bool) -> list[str]:
    """What is wrong with the suite's files, without running anything (empty: nothing)."""
    want = n_params(query_sql)
    problems: list[str] = []
    tuples = read_params(suite / PARAMS)
    if want and not tuples:
        problems.append(f"{PARAMS}: no tuples")
    problems += [f"{PARAMS} tuple {i} has {len(split_tuple(t))} values, the query takes {want}: {t}"
                 for i, t in enumerate(tuples, 1) if len(split_tuple(t)) != want]
    try:
        manifest = _yaml(suite / MANIFEST)
    except ValueError as exc:
        return problems + [str(exc)]
    if want:
        try:
            domain = _yaml(suite / DOMAIN)
            bad = _check_domain(domain, param_names, want)
            problems += bad
            if not bad:
                problems += [f"{DOMAIN} draws a tuple of {len(split_tuple(t))} values, the query takes {want}: {t}"
                             for t in draw_tuples(domain, 3, 1) if len(split_tuple(t)) != want]
        except ValueError as exc:
            problems.append(str(exc))
        problems += _check_edges(manifest, len(tuples))
    if not isinstance(manifest.get("rationale"), str) or not manifest["rationale"].strip():
        problems.append(f"{MANIFEST}: rationale string required")
    if not aux:
        return problems
    return problems + _validate_writes(suite, manifest, dependent_tables)


def _validate_writes(suite: Path, manifest: dict[str, Any], dependent_tables: list[str]) -> list[str]:
    problems: list[str] = []
    txns = txn_files(suite)
    declared = manifest.get("txns")
    if not txns:
        return [f"{TXNS}/: no write transactions"]
    if not isinstance(declared, dict) or set(declared) != set(txns):
        return [f"{MANIFEST} `txns` must list exactly the files of {TXNS}/ with a kind each"]
    kinds = {name: (spec or {}).get("kind") for name, spec in declared.items()}
    problems += [f"{MANIFEST} txns.{name}.kind must be one of {TXN_KINDS}" for name, k in kinds.items() if k not in TXN_KINDS]
    by_table: dict[str, set[str]] = {}
    for sql in txns.values():
        for verb, table in statements(sql):
            by_table.setdefault(table, set()).add(verb)
    waived = manifest.get("not_applicable") or {}
    for table in dependent_tables:
        for verb in VERBS:
            key = f"{table.lower()}:{verb}"
            if verb not in by_table.get(table.lower(), set()) and not str(waived.get(key, "")).strip():
                problems.append(f"no transaction runs {verb} on dependent table {table}: add one, or give "
                                f"not_applicable['{key}']: <why it cannot change this query's answer>")
    for kind in ("rollback", "key_move"):
        if kind not in kinds.values() and not str(waived.get(kind, "")).strip():
            problems.append(f"no `{kind}` transaction: add one, or give not_applicable['{kind}']: <why>")
    for name, sql in txns.items():
        if kinds.get(name) == "rollback" and not is_rollback(sql):
            problems.append(f"{TXNS}/{name} is a rollback transaction: its first line must be `{ROLLBACK_MARK}`")
    for key, label in (("point_writes", "point"), ("bulk_writes", "bulk")):
        if not isinstance(manifest.get(key), dict):
            problems.append(f"{MANIFEST}: {key} object required")
    point, bulk = manifest.get("point_writes") or {}, manifest.get("bulk_writes") or {}
    if isinstance(point, dict) and not (3 <= int(point.get("min_count", 0)) <= int(point.get("max_probes", 0)) <= 16):
        problems.append("point_writes needs 3 <= min_count <= max_probes <= 16")
    if isinstance(point, dict) and not point.get("tables"):
        problems.append("point_writes.tables must be a non-empty list")
    if isinstance(bulk, dict) and bulk.get("required") and int(bulk.get("min_chunks", 0)) < 1:
        problems.append("bulk_writes.min_chunks must be >= 1 when required")
    if isinstance(bulk, dict) and bulk.get("required") and not any(str(k).startswith("bulk_") for k in kinds.values()):
        problems.append("bulk_writes.required but no bulk_* transaction")
    if not manifest.get("scale_scenarios"):
        problems.append(f"{MANIFEST}: scale_scenarios: one entry per applicable failure class of scale_factor_contract.md")
    try:
        problems += _check_write_mix(_yaml(suite / WRITE_MIX), {str(k) for k in kinds.values()})
    except ValueError as exc:
        problems.append(str(exc))
    return problems


def validate_report(report: dict[str, Any], suite: Path, dependent_tables: list[str], aux: bool) -> list[str]:
    """What the run of the suite on plain SQL (the harness's validate mode) shows is wrong."""
    problems = [str(e) for e in report.get("errors") or []]
    manifest = _yaml(suite / MANIFEST)
    rows = report.get("tuple_rows") or []
    tuples = read_params(suite / PARAMS)
    edges = manifest.get("edge_coverage") or {}
    for i in edges.get("empty_result") if isinstance(edges.get("empty_result"), list) else []:
        if 1 <= i <= len(rows) and rows[i - 1] != 0:
            problems.append(f"edge_coverage.empty_result: tuple {i} returns {rows[i - 1]} rows, not none")
    for i in edges.get("null") if isinstance(edges.get("null"), list) else []:
        if 1 <= i <= len(tuples) and not re.search(r"\bnull\b", tuples[i - 1], re.I):
            problems.append(f"edge_coverage.null: tuple {i} contains no NULL")
    if not aux:
        return problems
    changes = {e["txn"]: int(e["changes_answers_of"]) for e in report.get("txn_effects") or []}
    for table in dependent_tables:
        touching = [n for n, sql in txn_files(suite).items() if any(t == table.lower() for _, t in statements(sql))]
        if touching and not any(changes.get(n, 0) > 0 for n in touching):
            problems.append(f"no transaction on {table} changes the answer of any tuple (checked: {', '.join(touching)}): "
                            "a stale maintenance of this table could not be caught; target rows that are in answers")
    return problems


# ---------------------------------------------------------------- the run of a tier


def quick_txn_names(suite: Path, count: int) -> list[str]:
    """The quick tier's transactions: the first of each distinct (kind, tables), in order, up to `count`."""
    declared = (_yaml(suite / MANIFEST).get("txns") or {})
    chosen, seen = [], set()
    for name, sql in txn_files(suite).items():
        key = ((declared.get(name) or {}).get("kind"), tuple(sorted({t for _, t in statements(sql)})))
        if key not in seen:
            seen.add(key)
            chosen.append(name)
    return chosen[:count]


def run_environment(suite: Path, *, tier: str, cfg: SuiteConfig, seed: int | None, params_file: Path) -> dict[str, str]:
    """The ACCEL_SUITE_* environment (host_contract.md, "Test suite") for one run of `tier` (quick, full or validate) with `seed`;
    writes the run's random tuples to `params_file`."""
    seed = seed or fresh_seed()
    random_tuples = {"quick": cfg.quick_random_tuples, "validate": cfg.validation_random_tuples}.get(
        tier, cfg.acceptance_random_tuples)
    drawn = draw_tuples(_yaml(suite / DOMAIN), random_tuples, seed) if (suite / DOMAIN).is_file() else []
    params_file.parent.mkdir(parents=True, exist_ok=True)
    params_file.write_text("\n".join(drawn) + ("\n" if drawn else ""), encoding="utf-8")
    env = {"ACCEL_SUITE_DIR": str(suite), "ACCEL_SUITE_PARAMS_FILE": str(params_file), "ACCEL_SUITE_SEED": str(seed),
           "ACCEL_SUITE_EVERY": "0" if tier == "quick" else str(cfg.differential_every)}
    if tier == "quick":
        env["ACCEL_SUITE_TXNS"] = ",".join(quick_txn_names(suite, cfg.quick_txns))
    if tier == "validate":
        env["ACCEL_SUITE_MODE"] = "validate"
    return env


# ---------------------------------------------------------------- freezing


def content_hash(suite: Path) -> str:
    digest = hashlib.sha256()
    for f in sorted(p for p in suite.rglob("*") if p.is_file() and "__pycache__" not in p.parts):
        digest.update(f.relative_to(suite).as_posix().encode() + b"\0" + f.read_bytes() + b"\0")
    return digest.hexdigest()


def freeze(art: Path, suite: Path, *, query: int, reason: str = "test_suite stage") -> dict[str, Any]:
    record = {"query": query, "hash": content_hash(suite), "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "reason": reason}
    (art / FROZEN_RECORD).write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record


def frozen_problem(art: Path, suite: Path) -> str:
    """'' when the suite is the one the test_suite stage froze; else what is wrong."""
    try:
        recorded = json.loads((art / FROZEN_RECORD).read_text(encoding="utf-8"))["hash"]
    except (OSError, ValueError, KeyError):
        return f"the suite {suite} was never frozen: run the test_suite stage"
    if content_hash(suite) != recorded:
        return (f"the frozen test suite {suite} was edited (hash differs from {FROZEN_RECORD}). It is only changed by "
                f"re-running the tester on a change request ({CHANGE_REQUEST} in the artifacts), never edited")
    return ""


def pending_change_request(art: Path) -> str:
    path = art / CHANGE_REQUEST
    return path.read_text(encoding="utf-8").strip() if path.is_file() else ""


def archive_change_request(art: Path, outcome: str) -> None:
    """Move the request to test_suite_change_requests/<n>.md with the tester's outcome."""
    path = art / CHANGE_REQUEST
    log = art / "test_suite_change_requests"
    log.mkdir(exist_ok=True)
    n = len(list(log.glob("*.md"))) + 1
    (log / f"{n:02d}.md").write_text(f"{path.read_text(encoding='utf-8').strip()}\n\n## Outcome\n\n{outcome}\n", encoding="utf-8")
    path.unlink()
