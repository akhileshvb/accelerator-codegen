#!/usr/bin/env bash
# Build engine adapter library. Host vs docker comes from gate-context host_runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-$ROOT/build-release}}"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_build.json}"
export PYTHONPATH="${ROOT}/pipeline/orchestrator${PYTHONPATH:+:$PYTHONPATH}"

python3 - "$ROOT" "$BUILD_DIR" "$CTX" "$OUT_JSON" <<'PY'
import json, os, subprocess, sys
from pathlib import Path

repo = Path(sys.argv[1])
build_dir = Path(sys.argv[2])
ctx_path = sys.argv[3]
out = Path(sys.argv[4])

status = "pass"
notes: list[str] = []
log_tail = ""
engine = ""
kind = "host_cmake"
data: dict = {}

if ctx_path and Path(ctx_path).is_file():
    data = json.loads(Path(ctx_path).read_text(encoding="utf-8"))
    engine = str(data.get("engine_name") or "")
    kind = str((data.get("host_runtime") or {}).get("kind") or "host_cmake")
else:
    status = "fail"
    notes.append("no gate context")


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


host = dict(data.get("host_runtime") or {})
if status == "pass" and kind == "docker":
    ensure_script = str(host.get("ensure_script") or "")
    extra_env = {str(k): str(v) for k, v in dict(host.get("env") or {}).items()}
    if not ensure_script:
        status = "fail"
        notes.append("host_runtime.kind=docker but ensure_script is empty")
    else:
        env = dict(os.environ)
        env.update(extra_env)
        # engine_build only needs the Docker host up; MODULE/plugin install is later layers.
        env.setdefault("ACCEL_PG_ENSURE_BUILD", "0")
        env.setdefault("ACCEL_MYSQL_ENSURE_BUILD", "0")
        ensure = run(["bash", str(repo / ensure_script)], timeout=3600.0, env=env)
        log_tail = ((ensure.stdout or "") + (ensure.stderr or ""))[-4000:]
        if ensure.returncode != 0:
            status = "fail"
            notes.append("docker ensure script failed")
        else:
            notes.append(f"docker ensure engine={engine or 'unknown'}")
elif status == "pass":
    cfg = [
        "cmake", "-S", str(repo), "-B", str(build_dir),
        "-DACCEL_ENABLE_PARALLEL_TPCH=OFF", "-DACCEL_ENABLE_PARALLEL_AUX=OFF",
    ]
    build_flag = str((data.get("cmake") or {}).get("build_flag") or "")
    if build_flag:
        cfg.append(f"-D{build_flag}=ON")
    cfg_proc = run(cfg)
    if cfg_proc.returncode != 0:
        status = "fail"
        notes.append("cmake configure failed")
        log_tail = ((cfg_proc.stdout or "") + (cfg_proc.stderr or ""))[-4000:]
    else:
        build_proc = run(
            ["cmake", "--build", str(build_dir), "--target", "accelerator_codegen",
             "-j", str(os.cpu_count() or 4)]
        )
        log_tail = ((build_proc.stdout or "") + (build_proc.stderr or ""))[-4000:]
        if build_proc.returncode != 0:
            status = "fail"
            notes.append("cmake build failed for accelerator_codegen")
        else:
            notes.append(
                f"configured build-dir={build_dir} engine={engine or 'unknown'} "
                "target=accelerator_codegen"
            )

# The backend names no query: queries register from accelerators/<bench>/q<N>/.
if status == "pass" and engine in ("duckdb", "postgres", "mysql"):
    check = run([sys.executable, str(repo / "scripts" / "check_backend_query_free.py"), "--engine", engine])
    if check.returncode != 0:
        status = "fail"
        notes.append("per-query code in the backend (registries + generic dispatch only): "
                     + (check.stdout or "")[-1500:])

payload = {
    "gate": "engine_build",
    "status": status,
    "notes": "; ".join(notes) if notes else status,
    "engine": engine,
    "mode": kind,
    "log_tail": log_tail,
}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
raise SystemExit(0 if status == "pass" else 1)
PY
