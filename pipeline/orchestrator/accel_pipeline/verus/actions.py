"""Deterministic operations of the Verus pipeline (stages and MCP tools share them).

Every function returns a JSON-able dict with ``ok`` and enough detail for an
agent to act on (errors are trimmed).  Nothing here calls an agent.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from accel_pipeline.verus.context import VerusContext

GEN_FILES = ("gen_spec.rs", "gen_obligations.rs", "gen_world.rs", "gen_ffi.rs")
LIB_RS_REQUIRED = ("gen_spec", "gen_obligations", "kernel", "gen_world", "gen_ffi")


# ───────────────────────────── helpers ─────────────────────────────


def _run(cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
         timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    full_env = os.environ.copy()
    full_env.update(env or {})
    try:
        return subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=full_env, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout if isinstance(exc.stdout, str) else ""
        return subprocess.CompletedProcess(cmd, 124, out, f"timed out after {timeout}s")


def _tail(text: str, n: int = 6000) -> str:
    return text if len(text) <= n else "... [truncated]\n" + text[-n:]


@contextmanager
def file_lock(path: Path) -> Iterator[None]:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a+") as fh:
        fcntl.flock(fh, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(fh, fcntl.LOCK_UN)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_result(ctx: VerusContext, name: str, data: dict[str, Any]) -> Path:
    d = ctx.art / "gates"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{name}.json"
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return path


# ───────────────────────────── sql2verus ─────────────────────────────


def sql2verus_bin(ctx: VerusContext) -> Path:
    t = ctx.root / "src" / "verus" / "sql2verus"
    binary = Path(os.environ.get("CARGO_TARGET_DIR", t / "target")) / "release" / "sql2verus"
    proc = _run(["cargo", "build", "--release", "--quiet", "--manifest-path", str(t / "Cargo.toml")], timeout=1800)
    if proc.returncode != 0:
        raise RuntimeError(f"sql2verus build failed:\n{_tail(proc.stderr)}")
    return binary


def _sql2verus(ctx: VerusContext, out_dir: Path, *, inputs: Path | None) -> subprocess.CompletedProcess[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(sql2verus_bin(ctx)), "--schema", ctx.schema_sql, "--query", ctx.query_sql,
           "--out", str(out_dir / "gen_spec.rs")]
    if inputs is not None:
        cmd += ["--obligations", str(out_dir / "gen_obligations.rs"), "--inputs", str(inputs),
                "--kernel-stub", str(out_dir / "kernel_stub.rs"), "--world", str(out_dir / "gen_world.rs"),
                "--ffi", str(out_dir / "gen_ffi.rs"), "--ffi-header", str(out_dir / ctx.header_name),
                "--ffi-prefix", ctx.ffi_prefix]
    return _run(cmd, timeout=600)


def freeze_spec(ctx: VerusContext) -> dict[str, Any]:
    """Freeze A: the spec from the SQL alone (into the build tree and the workspace)."""
    tmp = ctx.art / "gen" / "spec"
    proc = _sql2verus(ctx, tmp, inputs=None)
    if proc.returncode != 0:
        return {"ok": False, "error": _tail(proc.stderr or proc.stdout)}
    for dest in (ctx.crate_dir / "src", ctx.ws / "spec", ctx.ws / "crate" / "src"):
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy2(tmp / "gen_spec.rs", dest / "gen_spec.rs")
    return {"ok": True, "spec": str(ctx.ws / "spec" / "gen_spec.rs"), "sha256": sha256_file(tmp / "gen_spec.rs")}


def check_inputs(ctx: VerusContext, inputs: Path | None = None) -> dict[str, Any]:
    """Would sql2verus accept these kernel inputs?  (dry run of freeze B)"""
    inputs = inputs or ctx.ws / "inputs" / "q.inputs"
    if not inputs.is_file():
        return {"ok": False, "error": f"no inputs file at {inputs}"}
    tmp = ctx.art / "gen" / "check"
    if tmp.exists():
        shutil.rmtree(tmp)
    proc = _sql2verus(ctx, tmp, inputs=inputs)
    if proc.returncode != 0:
        return {"ok": False, "error": _tail((proc.stderr or proc.stdout).strip(), 3000)}
    return {"ok": True, "obligations": (tmp / "gen_obligations.rs").read_text(encoding="utf-8")[:20000]}


def freeze_inputs(ctx: VerusContext) -> dict[str, Any]:
    """Freeze B: obligations, store adapter, C ABI, its header, kernel stub.

    The host glue is not generated: the glue stage's agent writes it against
    the frozen header (and the glue contract check and audit hold it to the
    declared inputs).
    """
    inputs = ctx.ws / "inputs" / "q.inputs"
    tmp = ctx.art / "gen" / "full"
    if tmp.exists():
        shutil.rmtree(tmp)
    proc = _sql2verus(ctx, tmp, inputs=inputs)
    if proc.returncode != 0:
        return {"ok": False, "error": _tail(proc.stderr or proc.stdout)}
    src = ctx.crate_dir / "src"
    src.mkdir(parents=True, exist_ok=True)
    ws_src = ctx.ws / "crate" / "src"
    ws_src.mkdir(parents=True, exist_ok=True)
    for f in GEN_FILES:
        shutil.copy2(tmp / f, src / f)
        shutil.copy2(tmp / f, ws_src / f)
    ctx.kit_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(tmp / ctx.header_name, ctx.kit_dir / ctx.header_name)
    (ctx.ws / "frozen").mkdir(parents=True, exist_ok=True)
    for f in (ctx.header_name, "kernel_stub.rs", "gen_obligations.rs"):
        shutil.copy2(tmp / f, ctx.ws / "frozen" / f)
    shutil.copy2(inputs, ctx.ws / "frozen" / "q.inputs")
    # the crate's skeleton: lib.rs and the kernel (the stub until written)
    lib = ws_src / "lib.rs"
    if not lib.is_file():
        lib.write_text(
            "// Verus kernel crate (pipeline).  gen_* are generated and frozen; the\n"
            "// kernel (kernel.rs and any modules it adds) is written by the agents.\n"
            "#![allow(unused_imports)]\n\n" + "".join(f"pub mod {m};\n" for m in LIB_RS_REQUIRED),
            encoding="utf-8")
    if not (ws_src / "kernel.rs").is_file():
        shutil.copy2(tmp / "kernel_stub.rs", ws_src / "kernel.rs")
    return {"ok": True, "hashes": frozen_hashes(ctx)}


def frozen_paths(ctx: VerusContext) -> list[Path]:
    paths = [ctx.crate_dir / "src" / f for f in GEN_FILES]
    paths += [ctx.kit_dir / ctx.header_name, ctx.ws / "frozen" / "q.inputs"]
    return [p for p in paths if p.exists()]


def frozen_hashes(ctx: VerusContext) -> dict[str, str]:
    return {str(p.relative_to(ctx.root) if p.is_relative_to(ctx.root) else p): sha256_file(p)
            for p in frozen_paths(ctx)}


def check_frozen(ctx: VerusContext, hashes: dict[str, str], tests_hash: str = "") -> list[str]:
    changed = []
    for rel, h in hashes.items():
        if rel.startswith("glue/"):
            p = ctx.ws / rel
        else:
            p = Path(rel) if Path(rel).is_absolute() else ctx.root / rel
        if not p.is_file() or sha256_file(p) != h:
            changed.append(rel)
    # the workspace copies must match the build tree's
    for f in GEN_FILES:
        a, b = ctx.crate_dir / "src" / f, ctx.ws / "crate" / "src" / f
        if a.is_file() and (not b.is_file() or sha256_file(a) != sha256_file(b)):
            changed.append(f"workspace crate/src/{f}")
    if tests_hash and tests_hash != tree_hash(ctx.ws / "tests"):
        changed.append("workspace tests/")
    return changed


def tree_hash(d: Path) -> str:
    h = hashlib.sha256()
    if d.is_dir():
        for p in sorted(d.rglob("*")):
            if p.is_file():
                h.update(str(p.relative_to(d)).encode())
                h.update(p.read_bytes())
    return h.hexdigest()


# ───────────────────────────── the crate ─────────────────────────────


def kernel_files(ctx: VerusContext) -> list[Path]:
    src = ctx.ws / "crate" / "src"
    return sorted(p for p in src.glob("*.rs") if p.name not in GEN_FILES)


def sync_crate(ctx: VerusContext) -> None:
    """Workspace kernel files -> build tree (the generated files stay frozen)."""
    src = ctx.crate_dir / "src"
    src.mkdir(parents=True, exist_ok=True)
    keep = {p.name for p in kernel_files(ctx)}
    for p in src.glob("*.rs"):
        if p.name not in GEN_FILES and p.name not in keep:
            p.unlink()
    for p in kernel_files(ctx):
        shutil.copy2(p, src / p.name)


def glue_files(ctx: VerusContext) -> list[Path]:
    return sorted((ctx.ws / "glue").glob("*.cpp")) + sorted((ctx.ws / "glue").glob("*.hpp"))


def sync_glue(ctx: VerusContext) -> None:
    """Workspace glue/ -> the kit dir (the header stays the generated one)."""
    ctx.kit_dir.mkdir(parents=True, exist_ok=True)
    keep = {p.name for p in glue_files(ctx)}
    for p in list(ctx.kit_dir.glob("*.cpp")) + list(ctx.kit_dir.glob("*.hpp")):
        if p.name not in keep:
            p.unlink()
    for p in glue_files(ctx):
        shutil.copy2(p, ctx.kit_dir / p.name)


def glue_hashes(ctx: VerusContext) -> dict[str, str]:
    return {f"glue/{p.name}": sha256_file(p) for p in glue_files(ctx)}


def configure_build(ctx: VerusContext) -> dict[str, Any]:
    """Configure the build dir (DuckDB engine) when it is not yet (a fresh tree)."""
    proc = _run(["cmake", "-S", str(ctx.root), "-B", str(ctx.build_dir), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DACCEL_CODEGEN_ENGINE=duckdb", "-DACCEL_CALIBRATE_AT_BUILD=OFF",
                 f"-DACCEL_VERUS_LIB_DIR={ctx.lib_dir}"], timeout=1800)
    return {"ok": proc.returncode == 0, "error": _tail(proc.stdout + proc.stderr, 3000)}


def _verus_env(ctx: VerusContext) -> dict[str, str]:
    env = {"ACCEL_VERUS_LIB_DIR": ctx.lib_dir}
    if ctx.setting("verus_bin"):
        env["VERUS_BIN"] = str(ctx.setting("verus_bin"))
    return env


_RESULTS = re.compile(r"verification results:: (\d+) verified, (\d+) errors")


def verify(ctx: VerusContext, *, compile_lib: bool = False, proofs: bool = True) -> dict[str, Any]:
    """Sync the workspace crate and verify it (and compile it to the kernel library).

    proofs=False (code first): compile without checking the proofs.
    """
    sync_crate(ctx)
    script = ctx.root / "scripts" / "build_verus_crate.sh"
    if not proofs:
        mode = ["--no-verify"]
    else:
        mode = [] if compile_lib else ["--verify-only"]
    cmd = ["bash", str(script), str(ctx.crate_dir), ctx.crate] + mode
    t0 = time.monotonic()
    proc = _run(cmd, env=_verus_env(ctx), timeout=float(ctx.setting("verify_timeout_sec", 3600)))
    seconds = round(time.monotonic() - t0, 1)
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    m = _RESULTS.findall(out)
    verified, errors = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
    if not proofs:
        verified, errors = 0, 0 if proc.returncode == 0 else -1
    ok = proc.returncode == 0 and errors == 0
    res: dict[str, Any] = {"ok": ok, "verified": verified, "errors": errors, "seconds": seconds}
    if not ok:
        # keep the error blocks, not the trigger notes
        blocks = [b for b in re.split(r"\n(?=error|warning: unused)", out) if b.startswith("error")]
        res["output"] = _tail("\n".join(blocks) or out, 12000)
    return res


def ensure_runner(ctx: VerusContext) -> dict[str, Any]:
    """The runner binary, built if missing (the SQL tool needs it before any
    accelerator exists; build_runner rebuilds it with the kernel and glue)."""
    if ctx.runner.is_file():
        return {"ok": True}
    with file_lock(Path(ctx.build_dir) / ".verus_build.lock"):
        if ctx.runner.is_file():
            return {"ok": True}
        proc = _run(["cmake", "--build", ctx.build_dir, "--target", "verus_kit_runner", "-j",
                     str(os.cpu_count() or 8)], timeout=7200)
    if proc.returncode != 0:
        return {"ok": False, "error": _tail(proc.stdout + proc.stderr, 4000)}
    return {"ok": True}


def build_runner(ctx: VerusContext, *, proofs: bool = True) -> dict[str, Any]:
    """Compile the kernel library, then the engine + runner (CMake picks up the kit glue).

    proofs=False: the kernel compiled without checking its proofs (code first).
    """
    lib = verify(ctx, compile_lib=True, proofs=proofs)
    if not lib["ok"]:
        return {"ok": False, "stage": "verify", **lib}
    sync_glue(ctx)
    if not glue_files(ctx):
        return {"ok": False, "stage": "build", "output": "no host glue: glue/*.cpp is empty"}
    with file_lock(Path(ctx.build_dir) / ".verus_build.lock"):
        proc = _run(["cmake", "--build", ctx.build_dir, "--target", "verus_kit_runner", "-j",
                     str(os.cpu_count() or 8)], timeout=7200)
    if proc.returncode != 0:
        errs = "\n".join(l for l in (proc.stdout + proc.stderr).splitlines() if "error" in l.lower())
        return {"ok": False, "stage": "build", "output": _tail(errs or proc.stdout + proc.stderr, 8000)}
    return {"ok": True, "verified": lib["verified"], "verify_seconds": lib["seconds"]}


# ───────────────────────────── runner ─────────────────────────────


def db_copy(ctx: VerusContext, which: str, tag: str) -> Path:
    src = Path(ctx.test_db if which == "test" else ctx.bench_db)
    d = Path(ctx.run_dir) / "dbs"
    d.mkdir(parents=True, exist_ok=True)
    dst = d / f"{tag}.duckdb"
    for p in (dst, Path(str(dst) + ".wal")):
        if p.exists():
            p.unlink()
    shutil.copy2(src, dst)
    return dst


def _runner(ctx: VerusContext, args: list[str], *, store: str = "verus", fault: str = "",
            timeout: float = 1800) -> dict[str, Any]:
    env = {"ACCEL_AUX_STORE": store, "ACCEL_VERUS_KIT_FAULT": fault}
    proc = _run([str(ctx.runner), *args], env=env, timeout=timeout)
    line = next((l for l in reversed(proc.stdout.splitlines()) if l.startswith("{")), "")
    try:
        data = json.loads(line) if line else {}
    except json.JSONDecodeError:
        data = {}
    if not data:
        data = {"error": _tail(proc.stderr or proc.stdout or f"exit {proc.returncode}", 4000)}
    data["exit"] = proc.returncode
    return data


def tests_dir(ctx: VerusContext) -> Path:
    return ctx.ws / "tests"


def validate_tests(ctx: VerusContext) -> dict[str, Any]:
    db = db_copy(ctx, "test", "validate")
    r = _runner(ctx, ["validate", str(db), str(tests_dir(ctx)), ctx.query_sql], timeout=1200)
    r["ok"] = r.get("exit") == 0 and r.get("txns", 0) > 0
    if r.get("txns", 0) == 0:
        r.setdefault("errors", []).append("tests/txns/ has no transactions")
    return r


def differential(ctx: VerusContext, *, store: str = "verus", fault: str = "") -> dict[str, Any]:
    db = db_copy(ctx, "test", f"diff_{store}_{fault.replace(':', '_') or 'none'}")
    every = str(ctx.setting("gates.differential_every", 10))
    r = _runner(ctx, ["differential", str(db), ctx.accel_id, str(tests_dir(ctx)), every], store=store, fault=fault)
    r["ok"] = r.get("exit") == 0 and "error" not in r
    return r


def snapshots(ctx: VerusContext, *, store: str = "verus", fault: str = "") -> dict[str, Any]:
    db = db_copy(ctx, "test", f"snap_{store}_{fault or 'none'}")
    r = _runner(ctx, ["snapshots", str(db), ctx.accel_id, str(tests_dir(ctx)),
                      str(ctx.setting("gates.snapshot_writes", 200)), str(ctx.setting("gates.snapshot_readers", 4)),
                      "1"], store=store, fault=fault)
    r["ok"] = r.get("exit") == 0 and "error" not in r
    return r


def fallback_rate(result: dict[str, Any]) -> float:
    ex = result.get("executes") or {}
    total = int(ex.get("hit", 0)) + int(ex.get("fallback", 0))
    return int(ex.get("fallback", 0)) / total if total else 1.0


def correctness(ctx: VerusContext, *, store: str = "verus", snapshots_too: bool = True) -> dict[str, Any]:
    """The frozen tests on one store: differential (+ concurrent snapshots)."""
    d = differential(ctx, store=store)
    res: dict[str, Any] = {"differential": d}
    ok = d["ok"]
    if snapshots_too and ok:
        s = snapshots(ctx, store=store)
        res["snapshots"] = s
        ok = ok and s["ok"]
    rate = fallback_rate(d)
    res["fallback_rate"] = round(rate, 3)
    if rate > float(ctx.setting("gates.max_fallback_rate", 0.25)):
        ok = False
        res["fallback_error"] = (f"{rate:.0%} of accelerator queries were answered with SQL: the kernel declines "
                                 "too often (execute returning None, or states missing)")
    res["ok"] = ok
    return res


def _decls(ctx: VerusContext) -> list[dict[str, Any]]:
    from accel_pipeline.verus.contract import parse_inputs

    return parse_inputs((ctx.ws / "frozen" / "q.inputs").read_text(encoding="utf-8"))


def glue_tables(ctx: VerusContext) -> list[str]:
    return [d["name"] for d in _decls(ctx) if d["kind"] == "delta"]


def fault_selfcheck(ctx: VerusContext) -> dict[str, Any]:
    """Each deliberate bug must be caught by the frozen tests (else they are too weak)."""
    faults = ["skip_apply"] + [f"drop_delta:{t}" for t in glue_tables(ctx)]
    if any(d["kind"] == "rescan" for d in _decls(ctx)):
        faults.append("no_rescan")
    results: dict[str, Any] = {}
    missed = []
    for f in faults:
        r = differential(ctx, fault=f)
        caught = int(r.get("mismatches", 0)) + int(r.get("order_mismatches", 0)) > 0
        results[f] = {"caught": caught, "mismatches": r.get("mismatches"), "error": r.get("error")}
        if not caught:
            missed.append(f)
    s = snapshots(ctx, fault="ignore_snapshot")
    caught = int(s.get("mismatches", 0)) > 0
    results["ignore_snapshot"] = {"caught": caught, "mismatches": s.get("mismatches")}
    if not caught:
        missed.append("ignore_snapshot")
    return {"ok": not missed, "missed": missed, "results": results}


def explain_fault(f: str) -> str:
    return {
        "skip_apply": "commits leave the state unchanged (no transaction's changes are applied)",
        "no_rescan": "rescans return no rows (rows the kernel asks to rescan never arrive)",
        "ignore_snapshot": "readers always see the newest version, not their snapshot's",
    }.get(f, f"the changes to table {f.split(':', 1)[-1]} are dropped" if f.startswith("drop_delta:") else f)


def sql(ctx: VerusContext, query: str, max_rows: int = 50) -> dict[str, Any]:
    built = ensure_runner(ctx)
    if not built["ok"]:
        return {"error": "could not build verus_kit_runner for SQL", "detail": built["error"]}
    return _runner(ctx, ["sql", ctx.test_db, query, str(max_rows)], timeout=300)


def glue_contract(ctx: VerusContext) -> dict[str, Any]:
    """Record what the glue delivers and compare it with the declared inputs (contract.py)."""
    from accel_pipeline.verus import contract

    db = db_copy(ctx, "test", "contract")

    def runner_call(args: list[str], env: dict[str, str]) -> dict[str, Any]:
        proc = _run([str(ctx.runner), *args], env=env, timeout=1800)
        line = next((l for l in reversed(proc.stdout.splitlines()) if l.startswith("{")), "")
        data = json.loads(line) if line else {"error": _tail(proc.stderr or proc.stdout, 3000)}
        if proc.returncode != 0 and "error" not in data:
            data["error"] = _tail(proc.stderr or proc.stdout, 3000)
        return data

    return contract.check(ctx, runner_call, lambda q: sql(ctx, q, 100000), tests_dir(ctx), db)


# ───────────────────────────── bench / profile ─────────────────────────────


def bench(ctx: VerusContext, mixes: list[float], *, store: str = "verus", tag: str = "bench",
          reps: int | None = None, ops: int | None = None) -> dict[str, Any]:
    cfg = ctx.setting("optimization.bench", {}) or {}
    db = db_copy(ctx, "bench", tag)
    lock = Path(ctx.pipeline_root) / "verus_runs" / ".bench.lock"
    with file_lock(lock):  # timed runs never overlap another run's
        r = _runner(ctx, ["bench", str(db), ctx.accel_id, str(tests_dir(ctx)), str(reps or cfg.get("reps", 5)),
                          ",".join(str(m) for m in mixes), str(ops or cfg.get("ops", 200))], store=store,
                    timeout=float(ctx.setting("bench_timeout_sec", 3600)))
    r["ok"] = r.get("exit") == 0 and "error" not in r
    return r


def score(result: dict[str, Any], ctx: VerusContext) -> float:
    """read_speedup^w_r * geomean(mix speedups)^w_w (the C++ aux loop's dual axis)."""
    cfg = ctx.setting("optimization.bench", {}) or {}
    wr, ww = float(cfg.get("read_score_weight", 0.75)), float(cfg.get("write_score_weight", 0.25))
    read = max(float(result.get("read_speedup") or 0.0), 1e-6)
    mixes = [max(float(m.get("speedup") or 0.0), 1e-3) for m in result.get("mixes") or []]
    write = 1.0
    if mixes:
        prod = 1.0
        for m in mixes:
            prod *= m
        write = prod ** (1.0 / len(mixes))
    return read ** wr * write ** ww


def profile(ctx: VerusContext, mixes: list[float] | None = None) -> dict[str, Any]:
    """perf profile of a short bench: hotspots by symbol, grouped (kernel / glue / engine)."""
    db = db_copy(ctx, "bench", "profile")
    data = Path(ctx.run_dir) / "perf.data"
    args = [str(ctx.runner), "bench", str(db), ctx.accel_id, str(tests_dir(ctx)), "3",
            ",".join(str(m) for m in (mixes or [0.05])), "100"]
    rec = _run(["perf", "record", "-F", "499", "-g", "-o", str(data), "--", *args],
               env={"ACCEL_AUX_STORE": "verus", "VERUS_KIT_ACCEL_ONLY": "1"}, timeout=3600)
    if rec.returncode != 0:
        return {"ok": False, "error": _tail(rec.stderr, 2000)}
    rep = _run(["perf", "report", "-i", str(data), "--stdio", "--no-children", "--sort", "symbol",
                "--percent-limit", "1"], timeout=600)
    hotspots = []
    for line in rep.stdout.splitlines():
        m = re.match(r"\s*([\d.]+)%\s+\[[.k]\]\s+(.*)$", line)
        if m:
            sym = m.group(2).strip()
            hotspots.append({"share": round(float(m.group(1)) / 100.0, 4), "symbol": sym[:200],
                             "area": _area(ctx, sym)})
    by_area: dict[str, float] = {}
    for h in hotspots:
        by_area[h["area"]] = round(by_area.get(h["area"], 0.0) + h["share"], 4)
    return {"ok": True, "hotspots": hotspots[:30], "by_area": by_area}


def _area(ctx: VerusContext, sym: str) -> str:
    if f"{ctx.crate}_verus" in sym or "verus_common" in sym or "verus_prooflib" in sym:
        return "kernel"
    if "verus_substrate" in sym:
        return "store"
    if "verus_kit" in sym or "gen_glue" in sym or "DuckVerusHost" in sym or "KitModule" in sym:
        return "glue"
    if "duckdb" in sym:
        return "engine"
    return "other"


def hotspot_name(h: dict[str, Any]) -> str:
    """A stable name for a hotspot: the kernel function path, or its area."""
    sym = h["symbol"]
    if h["area"] == "kernel":
        m = re.search(r"([a-z_0-9]+::[A-Za-z_0-9]+)(?:::h[0-9a-f]{16})?$", sym.split("<")[0])
        return m.group(1) if m else sym.split("(")[0][-80:]
    return h["area"]


# ───────────────────────────── trust / leaks ─────────────────────────────

FORBIDDEN = [
    (r"\bassume\s*\(", "assume(...) (an unproved fact)"),
    (r"\badmit\s*\(", "admit(...)"),
    (r"assume_new\s*\(", "Ghost::assume_new / Tracked::assume_new"),
    (r"#\s*!?\[\s*verifier::external_body", "#[verifier::external_body] (unverified code)"),
    (r"#\s*!?\[\s*verifier::external\b", "#[verifier::external]"),
    (r"external_fn_specification|assume_specification", "an assumed specification"),
    (r"\bunsafe\b", "unsafe code"),
    (r"extern\s+\"C\"|#\s*\[\s*no_mangle", "a C ABI item (the C ABI is generated)"),
    (r"\bstd::(fs|net|process|env)\b", "I/O or process access"),
]


def trusted_base(ctx: VerusContext) -> dict[str, Any]:
    """The kernel adds nothing to the trusted base, and the crate keeps the generated modules."""
    violations = []
    for p in kernel_files(ctx):
        for i, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("//", 1)[0]
            for pat, what in FORBIDDEN:
                if re.search(pat, code):
                    violations.append(f"{p.name}:{i}: {what}: {line.strip()[:160]}")
    lib = ctx.ws / "crate" / "src" / "lib.rs"
    text = lib.read_text(encoding="utf-8") if lib.is_file() else ""
    for m in LIB_RS_REQUIRED:
        if not re.search(rf"^\s*pub\s+mod\s+{m}\s*;", text, re.M):
            violations.append(f"lib.rs: missing `pub mod {m};`")
    return {"ok": not violations, "violations": violations[:50]}


def leak_check(ctx: VerusContext) -> dict[str, Any]:
    """Tripwire: kernel code resembling the excluded hand-written Verus kernels.

    Distinctive identifiers (not in the generated files, the verified
    libraries or this run's spec) and long identical lines shared with
    src/verus/q*/ kernels count; above the thresholds the audit flags the run.
    """
    excluded = [p for d in sorted((ctx.root / "src" / "verus").glob("q*/src")) for p in d.glob("*.rs")
                if not p.name.startswith("gen_") and p.name not in ("lib.rs", "ffi.rs")]
    if not excluded:
        return {"ok": True, "note": "no excluded kernels to compare with"}
    allowed_text = "\n".join(
        p.read_text(encoding="utf-8")
        for d in ("common", "substrate", "prooflib")
        for p in (ctx.root / "src" / "verus" / d / "src").glob("*.rs"))
    allowed_text += "\n".join(p.read_text(encoding="utf-8") for p in (ctx.ws / "crate" / "src").glob("gen_*.rs"))
    ident = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]{5,}\b")
    allowed = set(ident.findall(allowed_text))
    ex_text = "\n".join(p.read_text(encoding="utf-8") for p in excluded)
    distinctive = set(ident.findall(ex_text)) - allowed
    mine = "\n".join(p.read_text(encoding="utf-8") for p in kernel_files(ctx))
    shared_idents = sorted(distinctive & set(ident.findall(mine)))
    ex_lines = {l.strip() for l in ex_text.splitlines() if len(l.strip()) >= 40}
    shared_lines = sorted({l.strip() for l in mine.splitlines() if l.strip() in ex_lines})
    lim_i = int(ctx.setting("leak.max_shared_identifiers", 25))
    lim_l = int(ctx.setting("leak.max_shared_lines", 5))
    ok = len(shared_idents) <= lim_i and len(shared_lines) <= lim_l
    return {"ok": ok, "shared_identifiers": len(shared_idents), "examples": shared_idents[:20],
            "shared_lines": len(shared_lines), "line_examples": shared_lines[:5]}


# ───────────────────────────── prooflib / playbook ─────────────────────────────


def prooflib_publish(ctx: VerusContext, note: str) -> dict[str, Any]:
    """Publish the workspace's proof library: additions only, re-verified with every dependent."""
    ws_lib = ctx.ws / "prooflib" / "src" / "lib.rs"
    tree_lib = ctx.root / "src" / "verus" / "prooflib" / "src" / "lib.rs"
    with file_lock(Path(ctx.pipeline_root) / "verus_runs" / ".prooflib.lock"):
        old = tree_lib.read_text(encoding="utf-8")
        new = ws_lib.read_text(encoding="utf-8")
        removed = [l for l in old.splitlines() if l.strip() and l not in new.splitlines()]
        if removed:
            return {"ok": False, "error": "the proof library only grows: these lines were removed or changed",
                    "lines": removed[:20]}
        if new == old:
            return {"ok": True, "note": "no change"}
        backup = old
        tree_lib.write_text(new, encoding="utf-8")
        script = ctx.root / "scripts" / "build_verus_crate.sh"
        failures = []
        # every crate that can use the library (this run's and other runs')
        for crate_dir in sorted(ctx.root.glob("src/generated/*/q*/verus_kit/*/crate")):
            if (crate_dir / "src" / "lib.rs").is_file():
                proc = _run(["bash", str(script), str(crate_dir), crate_dir.parent.name, "--verify-only"],
                            env=_verus_env(ctx), timeout=3600)
                out = proc.stdout + proc.stderr
                m = _RESULTS.findall(out)
                if proc.returncode != 0 or not m or int(m[-1][1]) != 0:
                    failures.append({"crate": crate_dir.parent.name, "output": _tail(out, 3000)})
        if failures:
            tree_lib.write_text(backup, encoding="utf-8")
            return {"ok": False, "error": "a dependent no longer verifies; nothing published", "failures": failures}
        log = Path(ctx.pipeline_root) / "verus" / "prooflib_provenance.jsonl"
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({"ts": time.time(), "query": ctx.query, "workload": ctx.workload_name,
                                 "note": note, "added_lines": len(new.splitlines()) - len(old.splitlines())}) + "\n")
    return {"ok": True}


def playbook_append(ctx: VerusContext, section: str, title: str, body: str, evidence: str) -> dict[str, Any]:
    if section not in ("performance", "proofs"):
        return {"ok": False, "error": "section must be 'performance' or 'proofs'"}
    if len(evidence.strip()) < 20:
        return {"ok": False, "error": "evidence required: an A/B result, before/after verification time, or "
                                       "measurements (what was tried, numbers)"}
    path = Path(ctx.pipeline_root) / "verus" / "playbook" / f"{section}.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    with file_lock(path.with_suffix(".lock")):
        with path.open("a", encoding="utf-8") as fh:
            fh.write(f"\n## {title.strip()}\n\n{body.strip()}\n\n_Evidence ({ctx.benchmark} q{ctx.query}):_ "
                     f"{evidence.strip()}\n")
    return {"ok": True}


def backend_addition_record(ctx: VerusContext, engine: str, helper: str, reason: str) -> dict[str, Any]:
    path = Path(ctx.pipeline_root) / "artifacts" / "backend_additions" / f"{engine}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with file_lock(path.with_suffix(".lock")):
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({"ts": time.time(), "query": ctx.query, "helper": helper, "reason": reason}) + "\n")
    return {"ok": True}
