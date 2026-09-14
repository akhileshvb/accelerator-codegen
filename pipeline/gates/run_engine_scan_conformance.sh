#!/usr/bin/env bash
# Engine-agnostic adapter conformance: target paths + aux/txn substrate symbols.
# Base scan is engine-local (not shared DataChunk / ScanNativeBaseTable).
# Semantic architecture is judged by architecture_audit.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_scan.json}"

python3 - "$ROOT" "$CTX" "$OUT_JSON" <<'PY'
import json, re, sys
from pathlib import Path

repo = Path(sys.argv[1])
ctx_path = sys.argv[2]
out = Path(sys.argv[3])

status = "pass"
notes: list[str] = []
missing: list[str] = []
engine = ""
targets: dict = {}

if not ctx_path or not Path(ctx_path).is_file():
    status = "fail"
    notes.append("no gate context")
else:
    data = json.loads(Path(ctx_path).read_text(encoding="utf-8"))
    engine = str(data.get("engine_name") or "")
    targets = dict(data.get("target_paths") or {})

def check_path(label: str, rel: str | None, *, want_file: bool) -> None:
    if not rel:
        missing.append(f"{label}=(unset)")
        return
    path = repo / rel
    ok = path.is_file() if want_file else path.exists()
    if not ok:
        missing.append(f"{label}={rel}")

if status == "pass":
    check_path("scan_adapter_header", targets.get("scan_adapter_header"), want_file=True)
    check_path("scan_adapter_source", targets.get("scan_adapter_source"), want_file=True)
    check_path("stateful_dir", targets.get("stateful_dir"), want_file=False)
    check_path("stateful_src_dir", targets.get("stateful_src_dir"), want_file=False)

    search_roots: list[Path] = []
    for key in ("scan_adapter_header", "scan_adapter_source", "stateful_dir", "stateful_src_dir"):
        rel = targets.get(key) or ""
        if not rel:
            continue
        p = repo / rel
        search_roots.append(p if p.is_dir() else p.parent)

    texts: list[str] = []
    for base in search_roots:
        if not base.exists():
            continue
        paths = [base] if base.is_file() else [
            p for p in base.rglob("*") if p.is_file() and p.suffix in {".hpp", ".h", ".cpp", ".cc", ".c"}
        ]
        for path in paths:
            try:
                texts.append(path.read_text(encoding="utf-8", errors="ignore"))
            except OSError:
                pass
    blob = "\n".join(texts)
    if blob:
        if not re.search(r"\b(AuxEngineServices|AuxStateStore)\b", blob):
            missing.append("missing symbol AuxEngineServices|AuxStateStore")
        if not re.search(r"\bMutationBatch\b", blob):
            missing.append("missing symbol MutationBatch")
        # Engine-local rebuild scan helper (name may vary); reject obvious client-SQL-only adapters.
        if re.search(r"\bPQexec(Params)?\b", blob) and not re.search(
            r"\b(table_beginscan|table_open|ScanNative|NativeScan|heap_getnext)\b", blob
        ):
            missing.append("scan path looks client-SQL-only (no native table-AM symbols)")
    elif any(targets.values()):
        missing.append("no readable sources under target_paths")

    if missing:
        status = "fail"
        notes.append("missing: " + "; ".join(missing))
    else:
        contract_rel = targets.get("contract_test") or (
            f"tests/engine_backend/test_{engine}_contract.cpp" if engine else ""
        )
        if contract_rel:
            cpath = repo / contract_rel
            if cpath.is_file():
                ctext = cpath.read_text(encoding="utf-8", errors="ignore")
                if re.search(
                    r"Fail\(\s*\"[^\"]*not implemented", ctext, flags=re.IGNORECASE
                ):
                    status = "fail"
                    missing.append(f"contract harness still has unimplemented stubs: {contract_rel}")
                    notes.append("missing: " + "; ".join(missing))
                else:
                    notes.append(
                        "target paths + AuxEngineServices/AuxStateStore/MutationBatch present "
                        "(full semantics: architecture_audit; no shared DataChunk requirement)"
                    )
            else:
                notes.append(
                    "target paths and substrate symbols present "
                    "(contract harness not on disk yet)"
                )
        else:
            notes.append(
                "target paths and substrate symbols present "
                "(semantic architecture checked by architecture_audit agent)"
            )

payload = {
    "gate": "engine_scan_conformance",
    "status": status,
    "notes": "; ".join(notes) if notes else status,
    "engine": engine,
    "targets": targets,
    "missing": missing,
}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
raise SystemExit(0 if status == "pass" else 1)
PY
