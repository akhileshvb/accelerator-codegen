#!/usr/bin/env bash
# Resume-loop: run tpch_pg_aux_all22 until all 22 aux queries pass DoD gates.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACK="${ROOT}/pipeline/config/packs/tpch_pg_aux_all22.yaml"
MODELS="${ROOT}/pipeline/config/models.codex.yaml"
OUT_DIR="${ROOT}/pipeline/artifacts/packs/tpch_pg_aux_all22"
mkdir -p "${OUT_DIR}"

ORCH="${ROOT}/pipeline/orchestrator"
# shellcheck disable=SC1091
source "${ORCH}/.venv/bin/activate"

check_done() {
  python3 - <<'PY'
import json
from pathlib import Path
root = Path("/home/akhilvb/accelerator-codegen")
ok = 0
detail = {}
for q in range(1, 23):
    aux_dir = root / f"accelerators/tpch/q{q}/postgres/aux"
    has_aux = aux_dir.is_dir() and any(aux_dir.rglob("*.cpp"))
    # Prefer engine-scoped artifacts under pipeline/artifacts/tpch/qN
    corr = base = speed = None
    for art_root in (
        root / f"pipeline/artifacts/tpch/q{q}/gates",
        root / f"pipeline/artifacts/q{q}/gates",
    ):
        for name, holder in (
            ("aux_correctness.json", "corr"),
            ("baseline.json", "base"),
            ("aux_speedup_verification.json", "speed"),
            ("aux_bench_sweep.json", "speed"),
        ):
            p = art_root / name
            if not p.is_file():
                continue
            try:
                data = json.loads(p.read_text())
            except Exception:
                continue
            # Ignore duckdb-era gate files for this campaign.
            eng = str(data.get("engine_name") or "").lower()
            if eng and eng != "postgres":
                continue
            if name.startswith("aux_correctness") and corr is None:
                corr = data
            elif name.startswith("baseline") and base is None:
                base = data
            elif speed is None and name.startswith("aux_"):
                speed = data
    corr_ok = bool(corr and corr.get("status") == "pass")
    base_ok = bool(base and base.get("status") == "pass")
    # Prefer baseline dual-axis for postgres; speedup_verification optional.
    speed_ok = True
    if speed is not None and speed.get("status") == "fail":
        speed_ok = False
    passed = has_aux and corr_ok and base_ok and speed_ok
    detail[q] = {
        "has_aux": has_aux,
        "aux_correctness": (corr or {}).get("status"),
        "baseline": (base or {}).get("status"),
        "passed": passed,
    }
    if passed:
        ok += 1
summary = {"passed": ok, "total": 22, "queries": detail}
out = root / "pipeline/artifacts/packs/tpch_pg_aux_all22/completion.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(summary, indent=2))
print(json.dumps({"passed": ok, "total": 22}, indent=2))
raise SystemExit(0 if ok == 22 else 1)
PY
}

iter=0
max_iters="${ACCEL_PG_AUX_MAX_ITERS:-100}"
while (( iter < max_iters )); do
  iter=$((iter + 1))
  echo "[until_done] iteration ${iter}/${max_iters}"
  if check_done; then
    echo "[until_done] 22/22 PASS"
    exit 0
  fi
  (
    cd "${ORCH}"
    python -m accel_pipeline.run_pack \
      --pack "${PACK}" \
      --models-config "${MODELS}" || true
  )
  sleep 5
done

echo "[until_done] exhausted iterations without 22/22" >&2
check_done || true
exit 1
