#!/usr/bin/env bash
# Build + run engine ↔ substrate contract harness.
# Postgres: Docker ensure/build + docker exec test binary (ACCEL_PG_DOCKER).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-$ROOT/build-release}}"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_contract.json}"
export PYTHONPATH="${ROOT}/pipeline/orchestrator${PYTHONPATH:+:$PYTHONPATH}"

python3 - "$ROOT" "$BUILD_DIR" "$CTX" "$OUT_JSON" <<'PY'
import json, os, subprocess, sys
from pathlib import Path

repo = Path(sys.argv[1])
build_dir = Path(sys.argv[2])
ctx_path = sys.argv[3]
out = Path(sys.argv[4])

_pg_note = os.environ.get("ACCEL_PG_DISCOVERY", "")
engine = ""
targets: dict = {}
status = "pass"
notes: list[str] = []
log_tail = ""

if ctx_path and Path(ctx_path).is_file():
    data = json.loads(Path(ctx_path).read_text(encoding="utf-8"))
    engine = str(data.get("engine_name") or "")
    targets = dict(data.get("target_paths") or {})
else:
    status = "fail"
    notes.append("no gate context")

contract_rel = targets.get("contract_test") or (
    f"backends/{engine}/tests/test_{engine}_contract.cpp" if engine else ""
)
cmake_target = targets.get("contract_test_target") or (
    f"test_{engine}_contract" if engine else ""
)


def run(cmd: list[str], *, timeout: float | None = None, env=None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(repo),
        capture_output=True,
        text=True,
        env=env or os.environ,
        timeout=timeout,
        start_new_session=True,
    )


if status == "pass":
    if not contract_rel or not (repo / contract_rel).is_file():
        status = "fail"
        notes.append(f"missing contract test source: {contract_rel or '(unset)'}")

if status == "pass" and engine == "postgres":
    ensure = run(["bash", str(repo / "scripts/ensure_accel_pg_test.sh")], timeout=3600.0)
    log_tail = ((ensure.stdout or "") + (ensure.stderr or ""))[-4000:]
    if ensure.returncode != 0:
        status = "fail"
        notes.append("docker ensure/build failed")
    else:
        container = os.environ.get("ACCEL_PG_CONTAINER") or "accel_pg_test"
        env = dict(os.environ)
        env["ACCEL_PG_NINJA_TARGETS"] = cmake_target
        build = run(
            ["bash", str(repo / "scripts/build_accel_postgres_extension.sh")],
            timeout=3600.0,
            env=env,
        )
        log_tail = ((build.stdout or "") + (build.stderr or ""))[-4000:]
        if build.returncode != 0:
            status = "fail"
            notes.append("docker extension/contract build failed")
        else:
            # Inside container: server on 5432; LOAD installed .so.
            cmd = [
                "docker", "exec",
                "-e", "ACCEL_PG_DOCKER=1",
                "-e", "PGHOST=127.0.0.1",
                "-e", "PGPORT=5432",
                "-e", f"PGUSER={os.environ.get('PGUSER', 'postgres')}",
                "-e", f"PGPASSWORD={os.environ.get('PGPASSWORD', 'postgres')}",
                "-e", "PGDATABASE=postgres",
                "-e", "ACCEL_POSTGRES_SO=/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so",
                container,
                f"/accel-build/accel/{cmake_target}",
            ]
            try:
                run_proc = run(cmd, timeout=300.0)
                log_tail = ((run_proc.stdout or "") + (run_proc.stderr or ""))[-4000:]
                if run_proc.returncode != 0:
                    status = "fail"
                    notes.append(f"contract harness failed via docker exec: {cmake_target}")
                else:
                    notes.append(f"ran {cmake_target} in {container}; exit 0")
                if _pg_note:
                    notes.append(_pg_note)
            except subprocess.TimeoutExpired as exc:
                status = "fail"
                notes.append(f"contract harness timed out after {exc.timeout}s")
                log_tail = ((exc.stdout or "") + (exc.stderr or ""))[-4000:]
elif status == "pass":
    cfg = [
        "cmake", "-S", str(repo), "-B", str(build_dir),
        "-DACCEL_ENABLE_PARALLEL_TPCH=OFF", "-DACCEL_ENABLE_PARALLEL_AUX=OFF",
    ]
    cfg_proc = run(cfg)
    if cfg_proc.returncode != 0:
        status = "fail"
        notes.append("cmake configure failed")
        log_tail = (cfg_proc.stdout + cfg_proc.stderr)[-4000:]
    else:
        build_proc = run(
            ["cmake", "--build", str(build_dir), "--target", cmake_target, "-j", str(os.cpu_count() or 4)]
        )
        if build_proc.returncode != 0:
            status = "fail"
            notes.append(f"build failed for target {cmake_target}")
            log_tail = (build_proc.stdout + build_proc.stderr)[-4000:]
        else:
            binary = build_dir / cmake_target
            if not binary.is_file():
                status = "fail"
                notes.append(f"contract binary missing after build: {cmake_target}")
            else:
                run_proc = run([str(binary)], timeout=180.0)
                log_tail = (run_proc.stdout + run_proc.stderr)[-4000:]
                if run_proc.returncode != 0:
                    status = "fail"
                    notes.append(f"contract harness failed: {cmake_target}")
                else:
                    notes.append(f"ran {cmake_target}; exit 0")

payload = {
    "gate": "engine_contract",
    "status": status,
    "notes": "; ".join(notes) if notes else status,
    "engine": engine,
    "mode": "docker" if engine == "postgres" else "host",
    "contract_test": contract_rel,
    "cmake_target": cmake_target,
    "log_tail": log_tail,
}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
raise SystemExit(0 if status == "pass" else 1)
PY
