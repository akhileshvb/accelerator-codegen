#!/usr/bin/env bash
# Build + run validation-owned adversarial harness.
# Host vs docker comes from gate-context host_runtime — not a hardcoded engine name.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-$ROOT/build-release}}"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_adversarial.json}"
export PYTHONPATH="${ROOT}/pipeline/orchestrator${PYTHONPATH:+:$PYTHONPATH}"

python3 - "$ROOT" "$BUILD_DIR" "$CTX" "$OUT_JSON" <<'PY'
import json, os, subprocess, sys
from pathlib import Path

from accel_pipeline.gate_capture import (
    describe_returncode,
    docker_exec_harness,
    local_harness,
    log_tail_for_agent,
    run_command,
)

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
returncode = None
host: dict = {}
data: dict = {}

if ctx_path and Path(ctx_path).is_file():
    data = json.loads(Path(ctx_path).read_text(encoding="utf-8"))
    engine = str(data.get("engine_name") or "")
    targets = dict(data.get("target_paths") or {})
    host = dict(data.get("host_runtime") or {})
else:
    status = "fail"
    notes.append("no gate context")

adv_rel = targets.get("adversarial_test") or (
    f"backends/{engine}/tests/test_{engine}_adversarial.cpp" if engine else ""
)
cmake_target = targets.get("adversarial_test_target") or (
    f"test_{engine}_adversarial" if engine else ""
)
kind = str(host.get("kind") or "host_cmake")

if status == "pass":
    if not adv_rel or not (repo / adv_rel).is_file():
        status = "fail"
        notes.append(f"missing adversarial test source: {adv_rel or '(unset)'}")

if status == "pass" and kind == "docker":
    ensure_script = str(host.get("ensure_script") or "")
    build_script = str(host.get("build_script") or "")
    ninja_env = str(host.get("ninja_targets_env") or "ACCEL_NINJA_TARGETS")
    container_env = str(host.get("container_env") or "")
    container = os.environ.get(container_env) if container_env else ""
    container = container or str(host.get("default_container") or "")
    binary_dir = str(host.get("binary_dir") or "/accel-build/accel")
    extra_env = {str(k): str(v) for k, v in dict(host.get("env") or {}).items()}
    if engine == "postgres":
        extra_env.setdefault("ACCEL_PG_DOCKER", "1")
        extra_env.setdefault("PGHOST", "127.0.0.1")
        extra_env.setdefault("PGPORT", "5432")
        extra_env.setdefault("PGUSER", os.environ.get("PGUSER", "postgres"))
        extra_env.setdefault("PGPASSWORD", os.environ.get("PGPASSWORD", "postgres"))
        extra_env.setdefault("PGDATABASE", "postgres")
        extra_env.setdefault(
            "ACCEL_POSTGRES_SO",
            "/usr/lib/postgresql/17/lib/accelerator_codegen_pg_test.so",
        )
    elif engine == "mysql":
        extra_env.setdefault("ACCEL_MYSQL_DOCKER", "1")
        extra_env.setdefault("MYSQL_HOST", "127.0.0.1")
        extra_env.setdefault("MYSQL_TCP_PORT", "3306")

    if ensure_script:
        ensure = run_command(
            ["bash", str(repo / ensure_script)], timeout=3600.0, cwd=str(repo)
        )
        log_tail = ensure.output[-4000:]
        returncode = ensure.returncode
        if ensure.returncode != 0:
            status = "fail"
            notes.append("host ensure script failed")
    if status == "pass" and build_script:
        env = dict(os.environ)
        env.update(extra_env)
        env[ninja_env] = cmake_target
        build = run_command(
            ["bash", str(repo / build_script)], timeout=3600.0, cwd=str(repo), env=env
        )
        log_tail = build.output[-4000:]
        returncode = build.returncode
        if build.returncode != 0:
            status = "fail"
            notes.append("docker adversarial build failed")
    if status == "pass":
        if not container:
            status = "fail"
            notes.append("docker host_runtime missing container name")
        else:
            binary = f"{binary_dir.rstrip('/')}/{cmake_target}"
            try:
                run_proc = docker_exec_harness(
                    container,
                    binary,
                    extra_env=extra_env,
                    timeout=300.0,
                    log_path=f"/tmp/accel_gate_{cmake_target}.log",
                )
                log_tail = run_proc.output[-4000:]
                returncode = run_proc.returncode
                if run_proc.returncode != 0:
                    status = "fail"
                    notes.append(
                        f"adversarial harness failed via docker exec: {cmake_target} "
                        f"({describe_returncode(run_proc.returncode)})"
                    )
                else:
                    notes.append(f"ran {cmake_target} in {container}; exit 0")
                if _pg_note:
                    notes.append(_pg_note)
            except subprocess.TimeoutExpired as exc:
                status = "fail"
                returncode = -1
                notes.append(f"adversarial harness timed out after {exc.timeout}s")
                log_tail = (((exc.stdout or "") + (exc.stderr or ""))[-4000:])
elif status == "pass":
    cfg = [
        "cmake", "-S", str(repo), "-B", str(build_dir),
        "-DACCEL_ENABLE_PARALLEL_KERNELS=OFF", "-DACCEL_ENABLE_PARALLEL_AUX=OFF",
    ]
    cfg_proc = run_command(cfg, cwd=str(repo))
    if cfg_proc.returncode != 0:
        status = "fail"
        returncode = cfg_proc.returncode
        notes.append("cmake configure failed")
        log_tail = cfg_proc.output[-4000:]
    else:
        build_proc = run_command(
            ["cmake", "--build", str(build_dir), "--target", cmake_target, "-j",
             str(os.cpu_count() or 4)],
            cwd=str(repo),
        )
        if build_proc.returncode != 0:
            status = "fail"
            returncode = build_proc.returncode
            notes.append(f"build failed for target {cmake_target}")
            log_tail = build_proc.output[-4000:]
        else:
            binary = build_dir / cmake_target
            if not binary.is_file():
                status = "fail"
                notes.append(f"adversarial binary missing after build: {cmake_target}")
            else:
                run_proc = local_harness([str(binary)], cwd=str(repo), timeout=180.0)
                log_tail = run_proc.output[-4000:]
                returncode = run_proc.returncode
                if run_proc.returncode != 0:
                    status = "fail"
                    notes.append(
                        f"adversarial harness failed: {cmake_target} "
                        f"({describe_returncode(run_proc.returncode)})"
                    )
                else:
                    notes.append(f"ran {cmake_target}; exit 0")

if status == "fail" and not str(log_tail).strip():
    log_tail = log_tail_for_agent(
        {"returncode": returncode, "notes": "; ".join(notes)},
        max_chars=4000,
    )

payload = {
    "gate": "engine_adversarial",
    "status": status,
    "notes": "; ".join(notes) if notes else status,
    "engine": engine,
    "mode": kind,
    "adversarial_test": adv_rel,
    "cmake_target": cmake_target,
    "returncode": returncode,
    "log_tail": log_tail,
}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
raise SystemExit(0 if status == "pass" else 1)
PY
