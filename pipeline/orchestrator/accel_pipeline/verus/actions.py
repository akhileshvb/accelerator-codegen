"""Deterministic operations of the Verus pipeline (stages and MCP tools share them).

Every function returns a JSON-able dict with ``ok`` and enough detail for an
agent to act on (errors are trimmed).  Nothing here calls an agent.
"""

from __future__ import annotations

import contextlib
from concurrent.futures import ThreadPoolExecutor
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

from accel_pipeline.layout import PROOFLIB
from accel_pipeline.verus import params as qparams
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
def file_lock(path: Path, *, shared: bool = False) -> Iterator[None]:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a+") as fh:
        fcntl.flock(fh, fcntl.LOCK_SH if shared else fcntl.LOCK_EX)
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


def cargo_target_dir(ctx: VerusContext) -> Path:
    """sql2verus's cargo build dir: shared across trees (outside them), so a fresh
    tree does not rebuild DataFusion."""
    d = ctx.setting("cargo_target_dir") or os.environ.get("CARGO_TARGET_DIR") \
        or str(Path.home() / ".cache" / "accel_verus" / "cargo-target")
    return Path(d)


def sql2verus_bin(ctx: VerusContext) -> Path:
    t = ctx.root / "substrate" / "verus" / "sql2verus"
    target = cargo_target_dir(ctx)
    target.mkdir(parents=True, exist_ok=True)
    binary = target / "release" / "sql2verus"
    proc = _run(["cargo", "build", "--release", "--quiet", "--manifest-path", str(t / "Cargo.toml")],
                env={"CARGO_TARGET_DIR": str(target)}, timeout=1800)
    if proc.returncode != 0:
        raise RuntimeError(f"sql2verus build failed:\n{_tail(proc.stderr)}")
    return binary


def _sql2verus(ctx: VerusContext, out_dir: Path, *, inputs: Path | None) -> subprocess.CompletedProcess[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    # the spec models the engine the accelerator runs on (e.g. how CHAR(n) compares)
    cmd = [str(sql2verus_bin(ctx)), "--engine", str(ctx.setting("engine", "duckdb")),
           "--schema", ctx.schema_sql, "--query", ctx.query_sql,
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
    # a new C ABI invalidates any installed glue: remove it, so the shared build never compiles old glue
    # against the new header (the glue stage writes and installs the new one)
    installed = ctx.kit_dir / ctx.header_name
    if not installed.is_file() or installed.read_bytes() != (tmp / ctx.header_name).read_bytes():
        for old in list(ctx.kit_dir.glob("*.cpp")) + list(ctx.kit_dir.glob("*.hpp")):
            old.unlink()
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


def _crate_hash(crate_dir: Path) -> str:
    h = hashlib.sha256()
    for p in sorted((Path(crate_dir) / "src").glob("*.rs")):
        h.update(p.name.encode() + b"\0" + p.read_bytes())
    return h.hexdigest()


def _verified_cache(ctx: VerusContext) -> Path:
    """Crates that last verified fully (crate dir -> its source hash): the prooflib's dependents."""
    return cargo_target_dir(ctx) / ".verified_crates.json"


def mark_verified(ctx: VerusContext, crate_dir: Path) -> None:
    path = _verified_cache(ctx)
    with file_lock(path.with_suffix(".lock")):
        data = json.loads(path.read_text()) if path.is_file() else {}
        data[str(Path(crate_dir).resolve())] = _crate_hash(crate_dir)
        path.write_text(json.dumps(data, indent=1))


def verify(ctx: VerusContext, *, compile_lib: bool = False, proofs: bool = True) -> dict[str, Any]:
    """Sync the workspace crate and verify it (and compile it to the kernel library).

    proofs=False (code first): compile without checking the proofs.
    """
    sync_crate(ctx)
    script = ctx.root / "scripts" / "build_verus_crate.sh"
    # compile_lib: the kit as a library for its benchmark's world (the one Verus
    # store for every accelerator of the database); update_world then puts it there
    if compile_lib:
        mode = ["--rlib"] if proofs else ["--rlib-no-verify"]
    else:
        mode = ["--verify-only"] if proofs else ["--no-verify"]
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
    if ok and proofs:
        mark_verified(ctx, ctx.crate_dir)
    if ok and compile_lib:
        world = update_world(ctx)
        res["world"] = world
        if not world["ok"]:
            res["ok"] = False
            res["output"] = world["output"]
        return res
    if not ok:
        # keep the error blocks, not the trigger notes; list EVERY error first (one line each,
        # where and what) so a long report's tail never hides the first failures
        blocks = [b for b in re.split(r"\n(?=error|warning: unused)", out)
                  if b.startswith("error") and not b.startswith("error: aborting")]
        index = []
        for b in blocks:
            head = b.splitlines()[0][:160]
            at = re.search(r"-->\s*(\S+)", b)
            index.append(f"- {Path(at.group(1)).name if at else '?'}: {head}")
        res["error_list"] = index
        listing = f"{len(index)} errors:\n" + "\n".join(index) + "\n\nDetails (first errors first):\n"
        details = "\n".join(blocks) or out
        res["output"] = listing + details[: max(2000, 12000 - len(listing))]
    return res


def update_world(ctx: VerusContext) -> dict[str, Any]:
    """Put the kit (its library, just compiled) into its benchmark's Verus world
    and rebuild it: the world is verified against every kit and compiled to the
    archive the engines link (CMake links a benchmark's kits only through it)."""
    t0 = time.monotonic()
    proc = _run(["bash", str(ctx.root / "scripts" / "update_verus_world.sh"), ctx.benchmark, ctx.crate],
                env=_verus_env(ctx), timeout=float(ctx.setting("verify_timeout_sec", 3600)))
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    m = _RESULTS.findall(out)
    res: dict[str, Any] = {"ok": proc.returncode == 0 and (not m or int(m[-1][1]) == 0),
                           "seconds": round(time.monotonic() - t0, 1)}
    if not res["ok"]:
        errs = [b for b in re.split(r"\n(?=error)", out) if b.startswith("error") and not b.startswith("error: aborting")]
        res["output"] = ("the Verus world (the one store for every accelerator of the database) does not verify or "
                         "build with this kernel:\n" + ("\n".join(errs) or _tail(out, 6000))[:8000])
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


def build_runner(ctx: VerusContext, *, proofs: bool = True, _foreign_retry: int = 0) -> dict[str, Any]:
    """Compile the kernel library, then the engine + runner (CMake picks up the kit glue).

    proofs=False: the kernel compiled without checking its proofs (code first).
    """
    lib = verify(ctx, compile_lib=True, proofs=proofs)
    if not lib["ok"]:
        return {"ok": False, "stage": "verify", **lib}
    if not glue_files(ctx):
        return {"ok": False, "stage": "build", "output": "no host glue: glue/*.cpp is empty"}
    with file_lock(Path(ctx.build_dir) / ".verus_build.lock"):
        # under the lock: another pipeline's build never compiles a half-copied glue
        before = {p.name: p.read_bytes() for p in ctx.kit_dir.glob("*.[ch]pp")} if ctx.kit_dir.is_dir() else {}
        sync_glue(ctx)
        proc = _run(["cmake", "--build", ctx.build_dir, "--target", "verus_kit_runner", "-j",
                     str(os.cpu_count() or 8)], timeout=7200)
        if proc.returncode != 0:
            # every pipeline links every query's glue: put back the last glue that built, so
            # this query's broken glue never fails another pipeline's build
            for p in ctx.kit_dir.glob("*.[ch]pp"):
                if p.name not in before:
                    p.unlink()
            for name, data in before.items():
                (ctx.kit_dir / name).write_bytes(data)
        else:
            freeze_runner(ctx, "current", locked=True)
    if proc.returncode != 0:
        errs = "\n".join(l for l in (proc.stdout + proc.stderr).splitlines() if "error" in l.lower())
        # every error in another query's files: not this kernel's failure -- wait for that query to fix
        # (or roll back) its glue and try again, rather than spend a fix round on it
        mine = str(ctx.kit_dir.resolve())
        paths = re.findall(r"(/\S+/accelerators/\S+?/verus_kit/[^/\s]+)/", errs)
        foreign = paths and all(not p.startswith(mine) for p in paths)
        retries = int(ctx.setting("build_foreign_retries", 15))
        if foreign and _foreign_retry < retries:
            time.sleep(60)
            return build_runner(ctx, proofs=proofs, _foreign_retry=_foreign_retry + 1)
        if foreign:
            errs = (f"the shared build fails in OTHER queries' files (not this kernel's), still after {retries} "
                    f"minutes of retries:\n{errs}")
        return {"ok": False, "stage": "build", "output": _tail(errs or proc.stdout + proc.stderr, 8000)}
    return {"ok": True, "verified": lib["verified"], "verify_seconds": lib["seconds"]}


# ───────────────────────────── runner ─────────────────────────────


def bench_lock(ctx: VerusContext) -> Path:
    """One lock for every timed run on the machine, across pipelines and trees (it
    lives in the shared cargo build dir, which every sandbox mounts), so parallel
    pipelines do not distort each other's measurements."""
    return cargo_target_dir(ctx) / ".bench.lock"


@contextmanager
def machine_slot(ctx: VerusContext, kind: str, slots: int, note=None) -> Iterator[int]:
    """One of `slots` machine-wide places for `kind` (e.g. optimization: its benchmarks take
    turns on one machine, so only a few pipelines optimize at once).  Waits for a free one."""
    d = cargo_target_dir(ctx) / f".{kind}_slots"
    d.mkdir(parents=True, exist_ok=True)
    waited = False
    while True:
        for i in range(max(slots, 1)):
            fh = open(d / f"{i}.lock", "a+")
            try:
                fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                fh.close()
                continue
            try:
                yield i
            finally:
                fcntl.flock(fh, fcntl.LOCK_UN)
                fh.close()
            return
        if not waited and note:
            note(f"waiting for one of {slots} machine-wide {kind} slots")
            waited = True
        time.sleep(30)


def drop_db(db: Path) -> None:
    """Remove a scratch database copy (an SF1 copy is ~300 MB; benches make many)."""
    for p in (db, Path(str(db) + ".wal")):
        p.unlink(missing_ok=True)


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


_NOT_NULL = re.compile(r"^\s*(\w+)\s+.*\bNOT\s+NULL\b", re.I)


def schema_constraints(ctx: VerusContext) -> Path:
    """The declared schema's NOT NULL columns as ALTER statements, for every database the
    runner opens: the test data then stays in the world the spec describes (the generated
    TPC-H databases do not declare these constraints themselves)."""
    out = Path(ctx.run_dir) / "schema_constraints.sql"
    stmts = []
    table = None
    for line in Path(ctx.schema_sql).read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*CREATE\s+TABLE\s+(\w+)", line, re.I)
        if m:
            table = m.group(1)
            continue
        if table and line.strip().startswith(")"):
            table = None
            continue
        c = _NOT_NULL.match(line)
        if table and c and c.group(1).upper() not in ("PRIMARY", "CONSTRAINT", "UNIQUE", "FOREIGN", "CHECK"):
            stmts.append(f"ALTER TABLE {table} ALTER COLUMN {c.group(1)} SET NOT NULL;")
    text = "\n".join(stmts) + ("\n" if stmts else "")
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")
    return out


def runner_env(ctx: VerusContext, **extra: str) -> dict[str, str]:
    return {"ACCEL_VERUS_SCHEMA_SQL": str(schema_constraints(ctx)), **extra}


def freeze_runner(ctx: VerusContext, name: str, *, locked: bool = False) -> Path:
    """A copy of the built runner and the library holding the kernel, so two
    kernels can be benchmarked alternately without rebuilding in between.
    `locked`: the caller holds the build lock already."""
    d = Path(ctx.run_dir) / "runners" / name
    tmp = d.with_name(name + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    with contextlib.nullcontext() if locked else file_lock(Path(ctx.build_dir) / ".verus_build.lock", shared=True):
        shutil.copy2(ctx.runner, tmp / ctx.runner.name)
        for lib in Path(ctx.build_dir).glob("libaccelerator_codegen.so*"):
            shutil.copy2(lib, tmp / lib.name)
    shutil.rmtree(d, ignore_errors=True)
    tmp.rename(d)
    return d


def current_runner(ctx: VerusContext) -> Path | None:
    """This pipeline's copy of the runner from its last successful build (build_runner
    freezes it): its tests run from it, so they neither hold the shared build lock
    (another pipeline's rebuild is never starved) nor see another pipeline's relink."""
    d = Path(ctx.run_dir) / "runners" / "current"
    return d if (d / ctx.runner.name).is_file() else None


def runner_exe(ctx: VerusContext, runner: Path | None = None) -> tuple[str, dict[str, str]]:
    """The runner binary to run (a frozen copy when there is one) and the environment it needs."""
    runner = runner or current_runner(ctx)
    if runner is None:
        return str(ctx.runner), {}
    lib_path = str(runner) + (":" + os.environ["LD_LIBRARY_PATH"] if os.environ.get("LD_LIBRARY_PATH") else "")
    return str(runner / ctx.runner.name), {"LD_LIBRARY_PATH": lib_path}


def _runner(ctx: VerusContext, args: list[str], *, store: str = "verus_global", fault: str = "",
            timeout: float = 1800, runner: Path | None = None, draw: qparams.Draw | None = None,
            draw_mode: str = "replace") -> dict[str, Any]:
    """`draw`: random parameter tuples from the domain (params.py); `replace` makes them the run's parameter
    sets (bench, profile), `extra` adds them to the frozen tests' (differential, snapshots)."""
    env = runner_env(ctx, ACCEL_AUX_STORE=store, ACCEL_VERUS_KIT_FAULT=fault, **qparams.runner_env(draw, draw_mode))
    exe, extra = runner_exe(ctx, runner)  # a frozen copy: its own kernel library first
    env.update(extra)
    # the shared binary only before this pipeline's first build (shared lock: never under a relink)
    with contextlib.nullcontext() if extra else file_lock(Path(ctx.build_dir) / ".verus_build.lock", shared=True):
        proc = _run([exe, *args], env=env, timeout=timeout)
    line = next((l for l in reversed(proc.stdout.splitlines()) if l.startswith("{")), "")
    try:
        data = json.loads(line) if line else {}
    except json.JSONDecodeError:
        data = {}
    if not data:
        data = {"error": _tail(proc.stderr or proc.stdout or f"exit {proc.returncode}", 4000)}
    data["exit"] = proc.returncode
    data.update(qparams.summary(draw))
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


RANDOM_PARAMS_MARK = "-- random parameter sets drawn from the domain when the tests were frozen"


def add_random_params(ctx: VerusContext) -> dict[str, Any]:
    """Append several random parameter tuples from the query's domain to tests/params.sql (once, before the
    tests are frozen), so the frozen tests cover the domain and not only the values the tests agent chose.
    The seed is recorded in the artifacts."""
    path = tests_dir(ctx) / "params.sql"
    text = path.read_text(encoding="utf-8") if path.is_file() else ""
    if RANDOM_PARAMS_MARK in text:
        return {"added": 0}
    draw = qparams.new_draw(ctx, "frozen_tests", sets=int(ctx.setting("gates.frozen_param_sets", 8)))
    if draw is None:
        return {"added": 0}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.rstrip("\n") + ("\n" if text.strip() else "") + RANDOM_PARAMS_MARK + "\n"
                    + "\n".join(draw.sets) + "\n", encoding="utf-8")
    return {"added": len(draw.sets), "seed": draw.seed}


def differential(ctx: VerusContext, *, store: str = "verus_global", fault: str = "") -> dict[str, Any]:
    """The frozen tests, plus a fresh random draw of parameter tuples from the domain on every run."""
    db = db_copy(ctx, "test", f"diff_{store}_{fault.replace(':', '_') or 'none'}")
    every = str(ctx.setting("gates.differential_every", 10))
    draw = qparams.new_draw(ctx, "differential", sets=int(ctx.setting("gates.random_param_sets", 6)))
    r = _runner(ctx, ["differential", str(db), ctx.accel_id, str(tests_dir(ctx)), every], store=store, fault=fault,
                draw=draw, draw_mode="extra")
    r["ok"] = r.get("exit") == 0 and "error" not in r
    return r


def snapshots(ctx: VerusContext, *, store: str = "verus_global", fault: str = "") -> dict[str, Any]:
    db = db_copy(ctx, "test", f"snap_{store}_{fault or 'none'}")
    draw = qparams.new_draw(ctx, "snapshots", sets=int(ctx.setting("gates.random_param_sets", 6)))
    r = _runner(ctx, ["snapshots", str(db), ctx.accel_id, str(tests_dir(ctx)),
                      str(ctx.setting("gates.snapshot_writes", 200)), str(ctx.setting("gates.snapshot_readers", 4)),
                      "1"], store=store, fault=fault, draw=draw, draw_mode="extra")
    r["ok"] = r.get("exit") == 0 and "error" not in r
    return r


def fallback_rate(result: dict[str, Any]) -> float:
    ex = result.get("executes") or {}
    total = int(ex.get("hit", 0)) + int(ex.get("fallback", 0))
    return int(ex.get("fallback", 0)) / total if total else 1.0


def correctness(ctx: VerusContext, *, store: str = "verus_global", snapshots_too: bool = True) -> dict[str, Any]:
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
    if rate > float(ctx.setting("gates.max_fallback_rate", 0.0)):
        ok = False
        res["fallback_error"] = (f"{rate:.0%} of accelerator queries were answered with SQL: inside the envelope "
                                 "the kernel may not decline, so a state went missing or execute returned None")
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
    # the other way round: when the kernel declines, the SQL fallback answers -- it must be right
    fb = differential(ctx, fault="decline_execute")
    fb_ok = fb.get("exit") == 0 and int(fb.get("mismatches", 0)) + int(fb.get("order_mismatches", 0)) == 0
    results["fallback_correct"] = {"ok": fb_ok, "mismatches": fb.get("mismatches"), "examples": fb.get("examples"),
                                   "error": fb.get("error")}
    if not fb_ok:
        missed.append("fallback_correct")
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
        "fallback_correct": "the SQL fallback (every read declined) gives wrong answers: a host/runtime bug, "
                            "not a test weakness",
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
        exe, extra = runner_exe(ctx)
        proc = _run([exe, *args], env=runner_env(ctx, **env, **extra), timeout=1800)
        line = next((l for l in reversed(proc.stdout.splitlines()) if l.startswith("{")), "")
        data = json.loads(line) if line else {"error": _tail(proc.stderr or proc.stdout, 3000)}
        if proc.returncode != 0 and "error" not in data:
            data["error"] = _tail(proc.stderr or proc.stdout, 3000)
        return data

    return contract.check(ctx, runner_call, lambda q: sql(ctx, q, 100000), tests_dir(ctx), db)


# ───────────────────────────── bench / profile ─────────────────────────────


def bench_size(ctx: VerusContext, mixes: list[float]) -> tuple[int, int]:
    """(reps, ops) for this query's benches: the configured size, scaled down so a full bench
    takes about `optimization.bench.target_sec` (a slow kernel or base query would otherwise
    hold the machine-wide bench lock for half an hour).  Probed once per run (10 ops, one mix)
    and kept, so every bench of the run -- baseline, paired, audit -- is the same size."""
    cfg = ctx.setting("optimization.bench", {}) or {}
    reps, ops = int(cfg.get("reps", 5)), int(cfg.get("ops", 200))
    cache = Path(ctx.run_dir) / "bench_size.json"
    if cache.is_file():
        try:
            d = json.loads(cache.read_text())
            return int(d["reps"]), int(d["ops"])
        except (ValueError, KeyError):
            pass
    target = float(cfg.get("target_sec", 180))
    db = Path(ctx.run_dir) / "dbs" / "size_probe.duckdb"
    try:
        with file_lock(bench_lock(ctx)):  # the copy too: no pipeline waits holding a 300 MB copy
            db = db_copy(ctx, "bench", "size_probe")
            t0 = time.monotonic()
            r = _runner(ctx, ["bench", str(db), ctx.accel_id, str(tests_dir(ctx)), "1", str(mixes[0]), "10"],
                        timeout=900)
            wall = time.monotonic() - t0
    finally:
        drop_db(db)
    if r.get("exit") != 0 or "error" in r:
        return reps, ops  # the probe failed: the configured size, not cached (the next bench re-probes)
    build = float(r.get("build_ms") or 0) / 1000.0
    per_op = max(wall - build, 0.05) / 10.0
    projected = reps * len(mixes) * ops * per_op
    if projected > target:
        ops = max(20, int(ops * target / projected))
        if reps * len(mixes) * ops * per_op > target and reps > 3:
            reps = 3
    cache.write_text(json.dumps({"reps": reps, "ops": ops, "probe_wall_s": round(wall, 1),
                                 "build_s": round(build, 1), "projected_full_s": round(projected, 1)}))
    return reps, ops


def cost_check(ctx: VerusContext) -> dict[str, Any]:
    """A quick SF1 measurement of a kernel that passes its tests: correct is not enough, it must also be
    fast at scale.  The tests' database is tiny, where per-write copies of the whole state or whole-state
    scans per read are invisible; on SF1 they make the accelerator slower than the plain query."""
    cfg = ctx.setting("cost_check", {}) or {}
    if not cfg.get("enabled", True):
        return {"ok": True, "skipped": True}
    mix = float(cfg.get("write_mix", 0.05))
    min_read, min_mix = float(cfg.get("min_read_speedup", 2.0)), float(cfg.get("min_mix_speedup", 1.0))
    def measure() -> dict[str, Any]:
        return bench(ctx, [mix], tag="cost_check", reps=1, ops=int(cfg.get("ops", 20)),
                     timeout=float(cfg.get("timeout_sec", 420)))

    def score(b: dict[str, Any]) -> tuple[float, float]:
        return float(b.get("read_speedup") or 0.0), float((b.get("mixes") or [{}])[0].get("speedup") or 0.0)

    r = measure()
    if r["ok"]:
        rd, mx = score(r)
        near = rd >= 0.85 * min_read and mx >= 0.85 * min_mix
        if (rd < min_read or mx < min_mix) and near:  # a near miss: re-measure once (a short run is noisy)
            r2 = measure()
            if r2["ok"] and min(score(r2)[0] / min_read, score(r2)[1] / min_mix) > min(rd / min_read, mx / min_mix):
                r = r2
    if not r["ok"]:
        return {"ok": False, "error": (
            f"the SF1 cost check did not finish ({_tail(str(r.get('error', '')), 600)}): {cfg.get('ops', 20)} reads/"
            f"writes on SF1 took longer than {cfg.get('timeout_sec', 420)} s, so some read or write does work in "
            "proportion to the whole state or a whole table")}
    read = float(r.get("read_speedup") or 0.0)
    mixed = float((r.get("mixes") or [{}])[0].get("speedup") or 0.0)
    # the cost a write adds over the plain write (a mix can hide a few very expensive writes)
    accel_w, plain_w = float(r.get("write_us_mean") or 0.0), float(r.get("plain_write_us_mean") or 0.0)
    added = max(accel_w - plain_w, 0.0)
    max_ratio, floor_us = float(cfg.get("max_write_overhead_ratio", 10.0)), float(cfg.get("write_overhead_floor_us", 5000))
    write_ok = plain_w <= 0 or added <= max(max_ratio * plain_w, floor_us)
    ok = read >= min_read and mixed >= min_mix and write_ok
    res = {"ok": ok, "read_speedup": round(read, 2), "write_mix": mix, "mix_speedup": round(mixed, 2),
           "plain_write_us": round(plain_w), "accel_write_us": round(accel_w), "build_ms": r.get("build_ms")}
    if not ok:
        res["error"] = (
            f"correct but too slow at SF1: reads {read:.2f}x the plain query (need >= {min_read}), a "
            f"{mix:.0%}-write mix {mixed:.2f}x (need >= {min_mix}). "
            + ("Reads do too much work: they should touch only what the query needs (the analysis's read path), "
               "not scan a structure or table. " if read < min_read else "")
            + ("Writes cost too much: each committed version must cost about its changes -- use a persistent "
               "structure (a write copies only the path to the keys it changes), not a collection cloned, "
               "merged or rebuilt per write, and not a list searched linearly (contracts/scale_principles.md)."
               if mixed < min_mix else "")
            + (f" Each write adds {added / 1000:.1f} ms of maintenance over the plain write's {plain_w / 1000:.1f} ms "
               f"(allowed: {max_ratio:g}x the plain write, or {floor_us / 1000:g} ms): a write must cost about its "
               "changes -- touch only the keys it changes, and update ordered indexes per changed entry."
               if not write_ok else ""))
    return res


def bench(ctx: VerusContext, mixes: list[float], *, store: str = "verus_global", tag: str = "bench",
          reps: int | None = None, ops: int | None = None, runner: Path | None = None,
          timeout: float | None = None, draw: qparams.Draw | None = None) -> dict[str, Any]:
    """`draw`: the random parameter tuples to time.  None: a fresh draw (a paired comparison passes
    one draw to both sides, so the pair sees the same parameters)."""
    if draw is None:
        draw = qparams.new_draw(ctx, tag)
    if reps is None and ops is None:
        reps, ops = bench_size(ctx, mixes)
    cfg = ctx.setting("optimization.bench", {}) or {}
    db = Path(ctx.run_dir) / "dbs" / f"bench_{tag}.duckdb"
    try:
        with file_lock(bench_lock(ctx)):  # timed runs never overlap another run's (any tree)
            db = db_copy(ctx, "bench", tag)  # inside the lock: no pipeline waits holding a 300 MB copy
            r = _runner(ctx, ["bench", str(db), ctx.accel_id, str(tests_dir(ctx)), str(reps or cfg.get("reps", 5)),
                              ",".join(str(m) for m in mixes), str(ops or cfg.get("ops", 200))], store=store,
                        timeout=timeout or float(ctx.setting("bench_timeout_sec", 1200)), runner=runner, draw=draw)
    finally:
        drop_db(db)
    r["ok"] = r.get("exit") == 0 and "error" not in r
    r["size"] = {"reps": reps, "ops": ops}
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
    """perf profiles of the steady state, one per axis of the score: `reads`
    (accelerated queries) and `maintenance` (writes applied through the
    accelerator). The build is a one-time cost and is not sampled: perf starts
    paused and the runner enables it after the build."""
    out: dict[str, Any] = {"ok": True, "note": "steady state only (build excluded); shares are per axis"}
    draw = qparams.new_draw(ctx, "profile")  # one fresh random draw for both axes
    out.update(qparams.summary(draw))
    phases = {"reads": ("reads", "3", "0", int(ctx.setting("optimization.profile_read_reps", 300))),
              "maintenance": ("writes", "1", "1.0", int(ctx.setting("optimization.profile_write_ops", 300)))}
    for axis, (phase, reps, mix, n) in phases.items():
        if phase == "reads":
            reps, ops = str(n), "1"
        else:
            ops = str(n)
        r = _profile_phase(ctx, axis, phase, reps, mix, ops, draw)
        if axis == "maintenance" and r.get("ok"):
            r.update(_write_overhead(ctx, draw=draw))
        out[axis] = r
        if not r.get("ok"):
            out["ok"] = False
    return out


def _write_overhead(ctx: VerusContext, ops: int = 50, draw: qparams.Draw | None = None) -> dict[str, Any]:
    """What the accelerator adds per write: the same writes plain vs applied
    through it (the engine executing the write itself is in both)."""
    db = db_copy(ctx, "bench", "profile_overhead")
    try:
        with file_lock(bench_lock(ctx)):
            exe, extra = runner_exe(ctx)
            p = _run([exe, "bench", str(db), ctx.accel_id, str(tests_dir(ctx)), "1", "1.0", str(ops)],
                     env=runner_env(ctx, ACCEL_AUX_STORE="verus_global", VERUS_KIT_BENCH_PHASE="writes",
                                    **qparams.runner_env(draw, "replace"), **extra),
                     timeout=3600)
    finally:
        drop_db(db)
    try:
        mx = json.loads(p.stdout.strip().splitlines()[-1])["mixes"][0]
    except (ValueError, KeyError, IndexError):
        return {"overhead_note": "could not measure: " + _tail(p.stderr, 500)}
    plain, accel = mx["base_us"] / ops, mx["accel_us"] / ops
    return {"plain_write_us": round(plain), "accel_write_us": round(accel),
            "overhead_us_per_write": round(accel - plain),
            "overhead_note": "accel_write_us - plain_write_us: the maintenance cost the accelerator adds; "
                             "the rest is the engine executing the write (not removable)"}


def _profile_phase(ctx: VerusContext, axis: str, phase: str, reps: str, mix: str, ops: str,
                   draw: qparams.Draw | None = None) -> dict[str, Any]:
    db = db_copy(ctx, "bench", f"profile_{axis}")
    run_dir = Path(ctx.run_dir)
    data = run_dir / f"perf_{axis}.data"
    ctl, ack = run_dir / f"perf_{axis}.ctl", run_dir / f"perf_{axis}.ack"
    for f in (ctl, ack):
        f.unlink(missing_ok=True)
        os.mkfifo(f)
    exe, extra = runner_exe(ctx)
    args = [exe, "bench", str(db), ctx.accel_id, str(tests_dir(ctx)), reps, mix, ops]
    env = runner_env(ctx, ACCEL_AUX_STORE="verus_global", VERUS_KIT_ACCEL_ONLY="1", VERUS_KIT_BENCH_PHASE=phase,
                     VERUS_KIT_PERF_CTL=str(ctl), VERUS_KIT_PERF_ACK=str(ack), **qparams.runner_env(draw, "replace"), **extra)
    try:
        with file_lock(bench_lock(ctx)):
            rec = _run(["perf", "record", "-F", "1999", "-g", "-D", "-1", "--control", f"fifo:{ctl},{ack}",
                        "-o", str(data), "--", *args], env=env, timeout=3600)
    finally:
        drop_db(db)
        for f in (ctl, ack):
            f.unlink(missing_ok=True)
    if rec.returncode != 0:
        return {"ok": False, "error": _tail(rec.stderr, 2000)}
    rep = _run(["perf", "report", "-i", str(data), "--stdio", "--no-children", "--sort", "symbol",
                "--percent-limit", "1"], timeout=600)
    hotspots = []
    for line in rep.stdout.splitlines():
        m = re.match(r"\s*([\d.]+)%\s+\[[.k]\]\s+(.*)$", line)
        if m:
            sym = re.sub(r"(\s+-)+\s*$", "", m.group(2)).strip()  # perf pads with empty columns ("  -  -")
            hotspots.append({"share": round(float(m.group(1)) / 100.0, 4), "symbol": sym[:140],
                             "area": _area(ctx, sym)})
    by_area: dict[str, float] = {}
    for h in hotspots:
        by_area[h["area"]] = round(by_area.get(h["area"], 0.0) + h["share"], 4)
    return {"ok": True, "hotspots": hotspots[:12], "by_area": by_area}


def _area(ctx: VerusContext, sym: str) -> str:
    if f"{ctx.crate}_verus" in sym or "verus_common" in sym or "verus_prooflib" in sym:
        return "kernel"
    if "verus_substrate" in sym or f"{ctx.benchmark}_world_verus" in sym:
        return "store"  # the substrate and the benchmark's world (the one Verus store)
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
    (r"exec_allows_no_decreases_clause", "a loop or function without a termination proof"),
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
    the excluded kernels (setting ``leak.excluded_glob``, repo-relative crate src dirs; the legacy
    hand-written kernels are gone, so none by default) count; above the thresholds the audit flags the run.
    """
    pattern = str(ctx.setting("leak.excluded_glob", "") or "")
    excluded = [p for d in (sorted(ctx.root.glob(pattern)) if pattern else []) for p in d.glob("*.rs")
                if not p.name.startswith("gen_") and p.name not in ("lib.rs", "ffi.rs")]
    if not excluded:
        return {"ok": True, "note": "no excluded kernels to compare with"}
    allowed_text = "\n".join(
        p.read_text(encoding="utf-8")
        for d in ("substrate/verus/common", "substrate/verus/store", PROOFLIB)
        for p in (ctx.root / d / "src").glob("*.rs"))
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


def prooflib_verify(ctx: VerusContext) -> dict[str, Any]:
    """Verify the workspace's copy of the proof library on its own (before publishing it)."""
    from accel_pipeline.verus import workspace as _ws

    _ws.refresh_prooflib(ctx)
    ws_lib = ctx.ws / "prooflib" / "src" / "lib.rs"
    lib_dir = Path(ctx.lib_dir)
    t0 = time.monotonic()
    proc = _run([str(ctx.setting("verus_bin")), str(ws_lib), "--crate-type=lib", "--crate-name=verus_prooflib",
                 "--import", f"verus_common={lib_dir / 'verus_common.vir'}",
                 "--extern", f"verus_common={lib_dir / 'libverus_common.rlib'}", "-L", f"dependency={lib_dir}"],
                env=_verus_env(ctx), timeout=1200)
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    m = _RESULTS.findall(out)
    verified, errors = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
    res: dict[str, Any] = {"ok": proc.returncode == 0 and errors == 0, "verified": verified, "errors": errors,
                           "seconds": round(time.monotonic() - t0, 1)}
    if not res["ok"]:
        blocks = [b for b in re.split(r"\n(?=error)", out) if b.startswith("error") and not b.startswith("error: aborting")]
        index = [f"- {b.splitlines()[0][:160]}" for b in blocks]
        res["output"] = f"{len(index)} errors:\n" + "\n".join(index) + "\n\n" + _tail("\n".join(blocks) or out, 9000)
    return res


def prooflib_publish(ctx: VerusContext, note: str) -> dict[str, Any]:
    """Publish the workspace's proof library: additions only, re-verified with every dependent."""
    from accel_pipeline.verus import workspace as _ws

    ws_lib = ctx.ws / "prooflib" / "src" / "lib.rs"
    tree_lib = _ws.prooflib_lib(ctx)  # accelerators/prooflib/src/lib.rs
    with file_lock(Path(ctx.pipeline_root) / "verus_runs" / ".prooflib.lock"):
        # take in what others published since this workspace's copy was made, keeping this
        # agent's additions (else a stale copy looks like it removes their lemmas)
        _ws.refresh_prooflib(ctx)
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
        # the library itself must verify
        lib_dir = Path(ctx.lib_dir)
        proc = _run([str(ctx.setting("verus_bin")), str(tree_lib), "--crate-type=lib", "--crate-name=verus_prooflib",
                     "--import", f"verus_common={lib_dir / 'verus_common.vir'}",
                     "--extern", f"verus_common={lib_dir / 'libverus_common.rlib'}", "-L", f"dependency={lib_dir}"],
                    env=_verus_env(ctx), timeout=600)
        if proc.returncode != 0:
            failures.append({"crate": "verus_prooflib", "output": _tail(proc.stdout + proc.stderr, 3000)})
        # every crate that verified fully before (unchanged since) must still verify; the others
        # (kernels mid-proof) pick the additions up at their next verify
        cache = json.loads(_verified_cache(ctx).read_text()) if _verified_cache(ctx).is_file() else {}
        deps = [d for d in sorted(ctx.root.glob("accelerators/*/q*/verus_kit/*/crate"))
                if (d / "src" / "lib.rs").is_file() and cache.get(str(d.resolve())) == _crate_hash(d)]

        def check(crate_dir: Path) -> dict[str, Any] | None:
            p = _run(["bash", str(script), str(crate_dir), crate_dir.parent.name, "--verify-only"],
                     env=_verus_env(ctx), timeout=600)
            out = p.stdout + p.stderr
            m = _RESULTS.findall(out)
            if p.returncode != 0 or not m or int(m[-1][1]) != 0:
                return {"crate": crate_dir.parent.name, "output": _tail(out, 3000)}
            return None

        if not failures:
            with ThreadPoolExecutor(max_workers=8) as pool:
                failures += [f for f in pool.map(check, deps) if f]
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
    """Log a request for the engine_backend run; query runs never write backends/ themselves."""
    path = Path(ctx.pipeline_root) / "artifacts" / "backend_additions" / f"{engine}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with file_lock(path.with_suffix(".lock")):
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({"ts": time.time(), "benchmark": ctx.benchmark, "query": ctx.query,
                                 "helper": helper, "reason": reason, "status": "requested"}) + "\n")
    return {"ok": True, "logged": str(path),
            "note": f"request logged for the {engine} engine_backend run; backends/{engine}/** is "
                    "not writable from a query run, so do not wait for or rely on the helper here"}
