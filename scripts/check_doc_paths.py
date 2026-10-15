#!/usr/bin/env python3
"""Report repo-relative paths mentioned in pipeline prompts/config/templates
and README.md files that do not exist.

Run from anywhere:  python3 scripts/check_doc_paths.py
Output: file:line: path  (-> instantiated form).  Exit 1 if anything dangles.
Placeholders that cannot be instantiated are listed on stderr (not failures).
"""
import fnmatch
import itertools
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOP_DIRS = ("substrate", "backends", "accelerators", "workloads", "eval",
            "pipeline", "scripts", "cmake")
SCAN_DIRS = ("pipeline/prompts", "pipeline/config", "pipeline/templates")
SKIP_PARTS = {".git", "artifacts", "worktrees"}  # plus any build*/ dir
EXTS = (".md", ".yaml", ".yml", ".tpl", ".json", ".txt")

# Placeholder name -> primary instantiation value, by kind.
NUM = ["query_number", "N", "n", "q", "query", "query_id"]          # -> 6
BENCH = ["bench", "benchmark", "benchmark_name"]                    # -> tpch
ENGINE = ["engine", "backend", "engine_name"]                       # -> duckdb
ENGINE_CAP = ["EngineName", "Engine"]                               # -> DuckDB
OTHER = {"layer": "capture", "layer_id": "capture", "other": "mysql", "worker_id": "0",
         "workload": "tpch_q6", "test_target": "test_tpch_q6"}
CAP = {"duckdb": "DuckDB", "postgres": "Postgres", "mysql": "MySQL"}
# A template is fine if ANY real query / bench / engine satisfies it (e.g.
# backends/<engine>/tests/test_<engine>_store.cpp). Primary value is tried first.
ALT_NUM = ["6", "1", "2", "3", "4", "5", "7", "8", "9", "10", "11", "12", "14", "15",
           "16", "17", "18", "19", "20", "21", "22", "24"]
ALT_BENCH = ["tpch", "sec_edgar"]
ALT_ENGINE = ["duckdb", "postgres", "mysql"]

# Paths that are legitimately absent: runtime state, generated outputs, gate
# names (not files), template stand-ins (q0), or prose. fnmatch'd on the path.
ALLOW = [
    "pipeline/artifacts/*", "pipeline/worktrees/*", "build*", "*/build*/*",
    "pipeline/prims_staging*", "pipeline/.locks/*", "pipeline/.codex/*",
    "pipeline/prims_provenance.jsonl", "pipeline/verus_runs/*",
    "pipeline/gates/*",              # gate ids in config, implemented in the orchestrator
    "cmake/ctest", "cmake/ninja",    # prose ("cmake/ctest")
    "accelerators/*/q0/*", "workloads/*/q0.sql",  # *_.template.yaml stand-ins
    "accelerators/*/q*/duckdb/base/single-threaded/fused_scan.cpp",  # optional, generated
    "accelerators/*/q*/duckdb/base/fused.hpp",                        # optional, generated
    "accelerators/*/qN/*",           # prose glob in pipeline/README
    "backends/*2",                   # host_bootstrap: "do not invent backends/<engine>2/"
    "scripts/ensure_*_host.sh", "scripts/build_*_extension.sh",       # engine_.template.yaml: new-engine scripts
    "backends/*/engine/scan_adapter.*", "backends/*/tests/test_*_contract.cpp",  # engine_backend templates' generated outputs
    "backends/*/tests/test_*_capture.cpp", "backends/*/tests/test_*_epochs.cpp",  # slot layer harnesses, generated
    "accelerators/*/q*/postgres/base", "accelerators/*/q*/postgres/base/*",     # pipeline-generated for postgres
    "scripts/run_aux_bench_sweep", "scripts/ensure_accel_*_test", "scripts/start_accel_*_test",
    "scripts/build_accel_*_plugin", "scripts/postgres_matview_*",     # gate command ids
]

TOKEN = re.compile(r"(?:[A-Za-z0-9_.<>/*|\-]|\{[^{}\s]*\}|\{\{[^{}\s]*\}\})+")
PLACEHOLDER = re.compile(r"\{\{?\s*(\w+)\s*\}?\}|<(\w+)>")


def instantiate(path, num="6", bench="tpch", engine="duckdb"):
    vals = {**OTHER, **dict.fromkeys(NUM, num), **dict.fromkeys(BENCH, bench),
            **dict.fromkeys(ENGINE, engine), **dict.fromkeys(ENGINE_CAP, CAP[engine])}
    missing = []

    def sub(m):
        name = m.group(1) or m.group(2)
        if name in vals:
            return vals[name]
        missing.append(name)
        return m.group(0)

    return PLACEHOLDER.sub(sub, path), missing


def expand(p):
    """Expand {a,b} and {a|b} alternations."""
    m = re.search(r"\{([^{}]*[,|][^{}]*)\}", p)
    if not m:
        return [p]
    return [x for alt in re.split(r"[,|]", m.group(1))
            for x in expand(p[:m.start()] + alt + p[m.end():])]


def exists(p):
    for c in (p, "accelerators/" + p):  # accelerators/ is an include root
        if any(ch in c for ch in "*?"):
            if next(ROOT.glob(c), None) is not None:
                return True
        elif (ROOT / c).exists():
            return True
    return False


def ok(p):
    return any(fnmatch.fnmatch(p, a) for a in ALLOW) or exists(p)


def is_repo_path(tok):
    if "/" in tok and tok.split("/", 1)[0] in TOP_DIRS:
        return True
    return bool(re.match(r"^(tpch|sec_edgar)/q[\w<>{}]+/", tok))


def satisfied(tok, primary):
    if all(ok(e) for e in expand(primary)):
        return True
    if primary == tok:  # not templated: no fallback
        return False
    for n, b, e in itertools.product(ALT_NUM, ALT_BENCH, ALT_ENGINE):
        inst, _ = instantiate(tok, n, b, e)
        if all(ok(x) for x in expand(inst)):
            return True
    return False


def files():
    for d in SCAN_DIRS:
        for f in sorted((ROOT / d).rglob("*")):
            if f.is_file() and f.suffix in EXTS:
                yield f
    for f in sorted(ROOT.rglob("README.md")):
        rel = f.relative_to(ROOT).parts
        if not (SKIP_PARTS & set(rel)) and not any(p.startswith("build") for p in rel):
            yield f


def main():
    bad, unresolved, seen = [], [], set()
    for f in files():
        rel = f.relative_to(ROOT)
        for ln, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
            line = re.sub(r"\w+://\S+", "", line)  # URLs
            for m in TOKEN.finditer(line):
                tok = m.group(0).strip(".,:;)(`'\"").lstrip("*").rstrip("/")
                if not tok or not is_repo_path(tok) or "..." in tok:
                    continue
                inst, miss = instantiate(tok)
                if miss:
                    unresolved.append(f"{rel}:{ln}: {tok} (no value for {sorted(set(miss))})")
                elif not satisfied(tok, inst) and (rel, ln, tok) not in seen:
                    seen.add((rel, ln, tok))
                    bad.append(f"{rel}:{ln}: {tok}" + (f"  (-> {inst})" if inst != tok else ""))
    print("\n".join(bad))
    for u in unresolved:
        print("UNINSTANTIABLE", u, file=sys.stderr)
    print(f"{len(bad)} dangling, {len(unresolved)} uninstantiable", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
