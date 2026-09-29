#!/usr/bin/env python3
"""Fail-closed post-generation verification driver.

Logos, the canonical-spec check, Rocq, Dafny, admit scans, Print Assumptions,
and the manifest are deterministic. One spec session writes the canonical
spec. The Rocq and Dafny sessions then run at the same time. A red proof
gate is sent back to that session for another round, up to --repair-rounds.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Callable


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_repo_env(root: Path) -> None:
    """Load accelerator-codegen/.env into the process environment.

    Existing variables win, so a shell export overrides the file. Values are
    not printed.
    """
    path = root / ".env"
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip("'").strip('"')
        if key and key not in os.environ:
            os.environ[key] = value


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha256_tree(root: Path) -> dict[str, str]:
    if not root.is_dir():
        return {}
    out: dict[str, str] = {}
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        out[str(path.relative_to(root))] = sha256_file(path)
    return out


def parse_print_assumptions(text: str) -> set[str] | None:
    """Return axiom names, empty if closed, or None if the output is unusable."""
    closed = "Closed under the global context" in text
    headers = {
        "Axioms",
        "Variables",
        "Section Variables",
        "Parameters",
        "Constants",
    }
    names: set[str] = set()
    in_section = False
    saw = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.endswith(":") and line[:-1] in headers:
            in_section = True
            saw = True
            continue
        if not line:
            in_section = False
            continue
        if in_section:
            tok = line.split()[0].rstrip(":")
            if tok and tok[0].isalpha():
                names.add(tok)
    if closed and not saw:
        return set()
    if not saw:
        return None
    return names


SPEC_FIELDS = (
    "query",
    "inputs",
    "parameters",
    "join",
    "filter",
    "group_key",
    "contributions",
    "aggregates",
    "final_projection",
    "ordering",
)


LIBRARY_PREFIXES = (
    "tests/verification/formalsql/lib/",
    "tests/verification/patterns/",
    "tests/verification/substrate/",
)


def catalog_path(root: Path) -> Path:
    return root / "tests" / "verification" / "patterns" / "CATALOG.json"


def load_catalog(root: Path) -> dict:
    path = catalog_path(root)
    if not path.is_file():
        return {"entries": []}
    body = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(body, dict) or not isinstance(body.get("entries"), list):
        raise ValueError(f"pattern catalog is not a list of entries: {path}")
    return body


def register_reuse(root: Path, query: str, session: str, record_path: Path) -> tuple[list[dict], str | None]:
    """Fold a session reuse record into the pattern catalog.

    A reusable addition must already live under the shared library. Query
    leaves are recorded for this run and are not copied into the catalog.
    Returns (entries added to the catalog, error).
    """
    if not record_path.is_file():
        return [], f"missing reuse record {record_path}"
    with _MANIFEST_LOCK:
        try:
            record = json.loads(record_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            return [], f"reuse record is not JSON: {exc}"
        if record.get("query") != query or record.get("session") != session:
            return [], "reuse record query or session does not match this run"
        additions = record.get("additions", [])
        if not isinstance(additions, list):
            return [], "reuse record additions must be a list"
        catalog = load_catalog(root)
        known = {entry.get("path") for entry in catalog["entries"]}
        added: list[dict] = []
        for item in additions:
            if not isinstance(item, dict):
                return [], "reuse addition must be an object"
            kind = item.get("kind")
            rel = str(item.get("path", ""))
            name = str(item.get("name", ""))
            if kind not in {"reusable", "query_leaf"} or not name or not rel:
                return [], "reuse addition needs name, kind, and path"
            target = (root / rel).resolve()
            try:
                target.relative_to(root.resolve())
            except ValueError:
                return [], f"reuse path escapes the repo: {rel}"
            if not target.is_file():
                return [], f"reuse path does not exist: {rel}"
            if kind == "query_leaf":
                continue
            if not rel.startswith(LIBRARY_PREFIXES):
                return [], f"reusable lemma is outside the pattern library: {rel}"
            if rel in known:
                continue
            entry = {
                "name": name,
                "kind": "reusable",
                "path": rel,
                "prover": item.get("prover", "rocq" if rel.endswith(".v") else "dafny"),
                "source_query": query,
                "generalizes": item.get("generalizes", ""),
            }
            catalog["entries"].append(entry)
            known.add(rel)
            added.append(entry)
        catalog_path(root).write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
        return added, None


def load_critique(path: Path) -> tuple[str, str]:
    """Return (verdict, feedback). A missing or invalid report is revise."""
    if not path.is_file():
        return "revise", f"missing critique report {path}"
    try:
        body = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return "revise", f"critique report is not JSON: {path}"
    verdict = body.get("verdict")
    findings = body.get("findings") or []
    if verdict not in {"accept", "revise"} or not isinstance(findings, list):
        return "revise", "critique verdict must be accept or revise"
    if verdict == "accept" and findings:
        listed = "\n".join(f"- {item}" for item in findings)
        return "revise", "critique cannot accept while findings remain\n" + listed
    feedback = str(body.get("feedback") or "")
    if findings:
        feedback = feedback + "\n" + "\n".join(f"- {item}" for item in findings)
    return verdict, feedback.strip()


def canonical_spec_path(root: Path, query: str) -> Path:
    return root / "tests" / "verification" / "specs" / query / "canonical_spec.json"


def canonical_spec_status(path: Path, query: str) -> str:
    """Return pass or fail. The spec is a neutral JSON document, not a proof."""
    if not path.is_file():
        return "fail"
    try:
        body = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return "fail"
    if not isinstance(body, dict):
        return "fail"
    query_field = body.get("query")
    if not isinstance(query_field, str) or not query_field.strip():
        return "fail"
    if any(field not in body for field in SPEC_FIELDS):
        return "fail"
    return "pass"


def allowlist_names(path: Path) -> set[str]:
    names: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names


QUERY_META = {
    "q1": {
        "qid": "tpch_q1",
        "accel_rel": "src/generated/tpch/q1/postgres/aux",
        "preconditions": [
            "Preconditions_Q1 db delta",
            "Q1NumericSafe db",
        ],
        "tcb": [
            {"name": "Logos lowering", "kind": "sql_lowering"},
            {"name": "FormalSQL reference semantics", "kind": "sql_lowering"},
            {"name": "NumericEmbedFaithful", "kind": "numeric_representation"},
            {"name": "AvgObservationFaithful", "kind": "numeric_representation"},
            {"name": "SpecExportCorrect", "kind": "rocq_dafny_spec"},
            {"name": "Corresponds_Q1 finite extract-diff", "kind": "cpp_model"},
            {"name": "AggsFaithful", "kind": "twin_spi"},
            {"name": "RebuildOracleCorrect (free Stage/load oracle)", "kind": "twin_spi"},
            {"name": "snapshot/SPI abstraction", "kind": "runtime_spi"},
            {"name": "Rocq/Dafny/Logos toolchain pins", "kind": "toolchain"},
        ],
        "semantic_residuals": [],
        "claim": (
            "Canonical status: tests/verification/q1/CURRENT_THEOREM_TCB.md. "
            "Under Preconditions_Q1 and Q1NumericSafe, and assuming "
            "NumericEmbedFaithful, FormalSQL_exec(EmbedDB_Q1(db), delta) "
            "observes EmbedResult(Eval_Spec_Q1_outs(cutoff(delta), db)) "
            "(= RelNode Q1Adapt ∘ Eval_Spec(LogicalSpec_Q1_rel, …); Q1ByteKeys "
            "discharged from Pre). Q1_GroupBagsPerm is Qed. Dafny proves Inv ⇒ "
            "Exec ≈_Q1 flat Eval_Spec_Q1_OutRows (via Eval_Spec_Q1_eq_EvalResultSpec; "
            "RelNode Dafny bridge blocked on map bag-order), "
            "maintenance preserves Inv, Hit ⇒ Aux execution, and the Twin↔Aux "
            "closed package (ComposeOutput≡ExecSeqM, BuiltInv from BuildState, "
            "HB1–HB4 bookkeeping). Intentional Twin SPI: AggsFaithful (agg "
            "BuildState only) and RebuildOracleCorrect on free Stage/load oracles "
            "(lineitem BuildState discharges RebuildOracleCorrect). "
            "C++ correspondence is cpp_model (finite extract-diff). "
            "Q1ExecCorrect alone does not cover ModuleTwin/C++."
        ),
    },
    "q12": {
        "qid": "tpch_q12",
        "accel_rel": "src/generated/tpch/q12/postgres/aux",
        "preconditions": [
            "Preconditions_Q12 orders lines",
            "forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l",
            "forall o, In o orders -> q12_priority_embed_ok (o_priority o)",
            "q12_sum_bound orders lines",
        ],
        "tcb": [
            {"name": "Logos lowering", "kind": "sql_lowering"},
            {"name": "FormalSQL reference semantics", "kind": "sql_lowering"},
            {"name": "SpecExportCorrect", "kind": "rocq_dafny_spec"},
            {"name": "Corresponds_Q12 unproved", "kind": "cpp_model"},
            {"name": "snapshot/SPI abstraction", "kind": "runtime_spi"},
            {"name": "Rocq/Dafny/Logos toolchain pins", "kind": "toolchain"},
        ],
        "semantic_residuals": [],
        "claim": (
            "Under Preconditions_Q12, pair-domain, priority-embed, and "
            "q12_sum_bound, FormalSQL_exec_Q12(EmbedDB_Q12(orders, lines)) "
            "observes EmbedResult(EvalSpec_Q12(S_Q12_catalog, orders, lines)). "
            "There is no Q12-specific semantic residual. Dafny proves "
            "Inv ⇒ ExecSeq = EvalResultSpec, maintenance preserves Inv, and "
            "Hit ⇒ that exact claim. Corresponds_Q12(C++, Model) is not proved. "
            "SpecExportCorrect is a pin, not a cross-prover theorem."
        ),
    },
    "q14": {
        "qid": "tpch_q14",
        "accel_rel": "src/generated/tpch/q14/postgres/aux",
        "generated": True,
        "preconditions": [],
        "tcb": [
            {"name": "Logos lowering", "kind": "sql_lowering"},
            {"name": "FormalSQL reference semantics", "kind": "sql_lowering"},
            {"name": "numeric ratio representation", "kind": "numeric_representation"},
            {"name": "SpecExportCorrect", "kind": "rocq_dafny_spec"},
            {"name": "Corresponds_Q14 unproved", "kind": "cpp_model"},
            {"name": "snapshot/SPI abstraction", "kind": "runtime_spi"},
            {"name": "Rocq/Dafny/Logos toolchain pins", "kind": "toolchain"},
        ],
        "semantic_residuals": [],
        "claim": (
            "Q14 promo revenue is the promotional extended-price sum divided by "
            "the total extended-price sum over the part-lineitem join in the "
            "shipdate month. The canonical spec, Rocq development, and Dafny "
            "model are produced by the agent sessions. A numeric-ratio "
            "representation boundary, SpecExportCorrect, and Corresponds_Q14 "
            "stay in the TCB. The semantic path is closed only when the "
            "generated proof manifest lists no query-specific residual."
        ),
    },
}


class GateRunner:
    def __init__(self, root: Path, run: Callable[[list[str]], subprocess.CompletedProcess[str]] | None = None):
        self.root = root
        self.run = run or self._run

    def _run(self, cmd: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            cmd,
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
        )

    def script(self, rel: str, *args: str) -> subprocess.CompletedProcess[str]:
        path = self.root / rel
        if path.suffix == ".py":
            cmd = [sys.executable, str(path), *args]
        else:
            cmd = ["bash", str(path), *args]
        return self.run(cmd)


def _ok(proc: subprocess.CompletedProcess[str], needle: str | None = None) -> bool:
    if proc.returncode != 0:
        return False
    if needle is None:
        return True
    text = (proc.stdout or "") + (proc.stderr or "")
    return needle in text


_MANIFEST_LOCK = threading.Lock()


def proof_manifest_path(root: Path, query: str) -> Path:
    return root / "tests" / "verification" / "formalsql" / query / "proof_manifest.json"


def merge_proof_manifests(root: Path, query: str) -> None:
    """Fold Rocq and Dafny side files into proof_manifest.json.

    The two sessions write different files so a parallel run cannot clobber
    the other prover's list. Missing side files leave the combined file as it is.
    """
    with _MANIFEST_LOCK:
        path = proof_manifest_path(root, query)
        combined = load_proof_manifest(root, query) or {}
        for side, keys in (
            ("rocq", ("require", "theorems", "rocq", "preconditions", "semantic_residuals")),
            ("dafny", ("dafny",)),
        ):
            side_path = path.with_name(f"proof_manifest.{side}.json")
            if not side_path.is_file():
                continue
            try:
                body = json.loads(side_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                continue
            if not isinstance(body, dict):
                continue
            for key in keys:
                if key in body:
                    combined[key] = body[key]
        if combined:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(combined, indent=2) + "\n", encoding="utf-8")


def load_proof_manifest(root: Path, query: str) -> dict | None:
    path = proof_manifest_path(root, query)
    if not path.is_file():
        return None
    try:
        body = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return body if isinstance(body, dict) else None


def _generated_admit_scan(root: Path, query: str) -> subprocess.CompletedProcess[str]:
    directory = root / "tests" / "verification" / "formalsql" / query
    files = sorted(directory.glob("*.v")) if directory.is_dir() else []
    if not files:
        return _failed(f"no Rocq sources under {directory}")
    pattern = re.compile(
        r"Admitted|(^|[^\w])admit([^\w]|$)|(^|[^\w])give_up([^\w]|$)|(^|\s)Axiom(\s|$)",
        re.M,
    )
    for path in files:
        if pattern.search(path.read_text(encoding="utf-8")):
            return _failed(f"Admitted/admit/Axiom in {path}")
    return subprocess.CompletedProcess(args=[], returncode=0, stdout="NO_ADMITTED OK\n", stderr="")


def _generated_spec_export(root: Path, query: str) -> subprocess.CompletedProcess[str]:
    manifest = load_proof_manifest(root, query)
    if not manifest:
        return _failed("missing proof_manifest.json")
    paths = manifest.get("rocq", [])
    if not isinstance(paths, list) or not paths:
        return _failed("proof_manifest.json lists no Rocq files")
    missing = [rel for rel in paths if isinstance(rel, str) and not (root / rel).is_file()]
    if missing:
        return _failed("missing spec files: " + ", ".join(missing))
    return subprocess.CompletedProcess(args=[], returncode=0, stdout="SPEC_EXPORT_CHECKED OK\n", stderr="")


def _failed(stderr: str) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=[], returncode=1, stdout="", stderr=stderr)


def sql_gates(query: str, runner: GateRunner) -> dict[str, str]:
    """Return gate name → pass|fail. Does not include LOGOS_OK."""
    fsql = "tests/verification/formalsql"
    if query == "q1":
        steps = {
            "SQL_SPEC_PROVED": [
                runner.script(f"{fsql}/scripts/compile_q1_proof.sh"),
                runner.script(f"{fsql}/scripts/compile_q1_normalize_sound.sh"),
            ],
            "NO_QUERY_SEMANTIC_RESIDUAL": [
                runner.script(f"{fsql}/scripts/check_q1_no_semantic_residual.sh"),
            ],
            "NO_ADMITTED": [
                runner.script(f"{fsql}/scripts/check_q1_rocq_no_admitted.sh"),
            ],
            "PRINT_ASSUMPTIONS": [
                runner.script(f"{fsql}/scripts/check_print_assumptions.sh", "q1"),
            ],
            "SPEC_EXPORT_CHECKED": [
                runner.script(f"{fsql}/resultspec/check_spec_export.sh"),
            ],
        }
        proved_needle = "NORMALIZE_SOUND_OK"
    elif query == "q12":
        normalize = runner.script(f"{fsql}/scripts/compile_q12_normalize_sound.sh")
        steps = {
            "SQL_SPEC_PROVED": [normalize],
            "NO_QUERY_SEMANTIC_RESIDUAL": [normalize],
            "NO_ADMITTED": [
                runner.script(f"{fsql}/scripts/check_q12_rocq_no_admitted.sh"),
            ],
            "PRINT_ASSUMPTIONS": [
                runner.script(f"{fsql}/scripts/check_print_assumptions.sh", "q12"),
            ],
            "SPEC_EXPORT_CHECKED": [
                runner.script(f"{fsql}/resultspec/check_spec_export_q12.sh"),
            ],
        }
        proved_needle = "Q12_SQL_SPEC_PROVED OK"
    elif QUERY_META[query].get("generated"):
        merge_proof_manifests(runner.root, query)
        script = f"tests/verification/formalsql/scripts/compile_{query}_normalize_sound.sh"
        normalize = (
            runner.script(script)
            if (runner.root / script).is_file()
            else _failed(f"missing {script}")
        )
        steps = {
            "SQL_SPEC_PROVED": [normalize],
            "NO_QUERY_SEMANTIC_RESIDUAL": [normalize],
            "NO_ADMITTED": [_generated_admit_scan(runner.root, query)],
            "PRINT_ASSUMPTIONS": [
                runner.script("tests/verification/formalsql/scripts/check_print_assumptions.sh", query)
            ],
            "SPEC_EXPORT_CHECKED": [_generated_spec_export(runner.root, query)],
        }
        proved_needle = f"Q{query[1:]}_SQL_SPEC_PROVED OK"
    else:
        raise ValueError(f"no SQL gates for {query}")

    status: dict[str, str] = {}
    for name, procs in steps.items():
        if name == "SQL_SPEC_PROVED":
            status[name] = "pass" if all(_ok(p) for p in procs) and _ok(procs[-1], proved_needle) else "fail"
        elif name == "NO_QUERY_SEMANTIC_RESIDUAL" and query != "q1":
            text = (procs[0].stdout or "") + (procs[0].stderr or "")
            needle = proved_needle if QUERY_META[query].get("generated") else "Q12_SQL_SPEC_PROVED OK"
            pending = "pending" in text or "residual remains" in text
            status[name] = "pass" if procs[0].returncode == 0 and not pending and needle in text else "fail"
        else:
            status[name] = "pass" if all(_ok(p) for p in procs) else "fail"
    return status


def _dafny(root: Path) -> list[str]:
    candidate = root / "tools/dafny/dafny"
    if candidate.is_file():
        return [str(candidate)]
    return ["dafny"]


def accel_gates(query: str, runner: GateRunner) -> dict[str, str]:
    root = runner.root
    dafny = _dafny(root)
    common = ["--allow-warnings"]

    def verify(rel: str, extra: list[str] | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [*dafny, "verify", str(root / rel), *common, *(extra or [])]
        return runner.run(cmd)

    if query == "q1":
        model = [
            verify("tests/verification/q1/dafny/generated/Q1AuxSpec.dfy"),
            verify("tests/verification/q1/dafny/Q1TwinSpecBridge.dfy"),
            verify("tests/verification/q1/dafny/Q1Finalize.dfy"),
            verify("tests/verification/q1/dafny/Q1ModuleTwin.dfy"),
            verify("tests/verification/q1/dafny/Q1Aux.dfy"),
            verify("tests/verification/q1/dafny/Q1TwinAuxRefinement.dfy"),
            verify("tests/verification/q1/dafny/Q1TwinAuxClose.dfy"),
            verify("tests/verification/q1/dafny/Q1ResultSpecBridge.dfy"),
        ]
        maint_files = [
            root / "tests/verification/q1/dafny/Q1Maintenance.dfy",
            root / "tests/verification/q1/dafny/Q1Aux.dfy",
        ]
        maint = [verify("tests/verification/q1/dafny/Q1Maintenance.dfy")]
        hit = [
            verify("tests/verification/substrate/dafny/VersionedPairing.dfy"),
            verify("tests/verification/substrate/dafny/SnapshotPinning.dfy"),
            verify("tests/verification/substrate/dafny/ReadHandle.dfy"),
            verify("tests/verification/q1/dafny/Q1RuntimeAuxCorrect.dfy"),
            verify("tests/verification/q1/dafny/Q1RuntimeResultSpec.dfy"),
        ]
        extract = runner.script("scripts/run_dafny_q1_extract_diff.sh")
        cpp = "finite_test" if _ok(extract) else "fail"
    elif query == "q12":
        model = [
            verify("tests/verification/patterns/dafny/SortedUniqueMap.dfy"),
            verify(
                "tests/verification/q12/dafny/Q12Aux.dfy",
                ["--isolate-assertions", "--cores", "2", "--verification-time-limit", "180"],
            ),
            verify(
                "tests/verification/q12/dafny/Q12ResultSpecBridge.dfy",
                ["--cores", "2", "--verification-time-limit", "120"],
            ),
        ]
        maint_files = [
            root / "tests/verification/q12/dafny/Q12Aux.dfy",
            root / "tests/verification/q12/dafny/Q12Maintenance.dfy",
        ]
        maint = [
            verify(
                "tests/verification/q12/dafny/Q12Maintenance.dfy",
                ["--cores", "2", "--verification-time-limit", "120"],
            )
        ]
        hit = [
            verify(
                "tests/verification/q12/dafny/Q12RuntimeResultSpec.dfy",
                ["--cores", "2", "--verification-time-limit", "120"],
            )
        ]
        cpp = "unproved"
    else:
        merge_proof_manifests(root, query)
        manifest = load_proof_manifest(root, query) or {}
        dafny_files = [rel for rel in manifest.get("dafny", []) if isinstance(rel, str)]
        model_files = [rel for rel in dafny_files if "Maintenance" not in rel and "Runtime" not in rel]
        maint_rels = [rel for rel in dafny_files if "Maintenance" in rel]
        hit_rels = [rel for rel in dafny_files if "Runtime" in rel]
        model = [verify(rel) for rel in model_files] or [_failed("proof_manifest.json lists no Dafny model")]
        maint = [verify(rel) for rel in maint_rels] or [_failed("proof_manifest.json lists no maintenance file")]
        hit = [verify(rel) for rel in hit_rels] or [_failed("proof_manifest.json lists no runtime file")]
        maint_files = [root / rel for rel in maint_rels]
        cpp = "unproved"

    axiom = "assume {:axiom}"
    axiom_hit = False
    for path in maint_files:
        if path.is_file() and axiom in path.read_text(encoding="utf-8"):
            axiom_hit = True
    return {
        "ACCEL_MODEL_PROVED": "pass" if all(_ok(p) for p in model) else "fail",
        "MAINTENANCE_PROVED": "fail" if axiom_hit or not all(_ok(p) for p in maint) else "pass",
        "SNAPSHOT_HIT_PROVED": "pass" if all(_ok(p) for p in hit) else "fail",
        "CPP_MODEL_CORRESPONDENCE": cpp,
    }


def gates_green(status: dict[str, str]) -> bool:
    return all(value in {"pass", "finite_test", "unproved", "skipped"} for value in status.values())


def format_gate_status(status: dict[str, str]) -> str:
    failed = [f"{name}: {value}" for name, value in status.items() if value not in {"pass", "finite_test", "unproved", "skipped"}]
    if not failed:
        return "All gates for this track are green."
    return "These gates are still red:\n" + "\n".join(f"- {line}" for line in failed)


def read_plan(path: Path) -> str:
    if not path.is_file():
        return "No plan yet. Write one before editing."
    return path.read_text(encoding="utf-8")


def fill_prompt(template: str, values: dict[str, str]) -> str:
    text = template
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", value)
    return text


def sql_gate_commands(query: str) -> str:
    if query == "q1":
        return "\n".join(
            [
                "bash tests/verification/formalsql/scripts/compile_q1_proof.sh",
                "bash tests/verification/formalsql/scripts/compile_q1_normalize_sound.sh",
                "bash tests/verification/formalsql/scripts/check_q1_rocq_no_admitted.sh",
                "bash tests/verification/formalsql/scripts/check_q1_no_semantic_residual.sh",
                "bash tests/verification/formalsql/scripts/check_print_assumptions.sh q1",
            ]
        )
    if query == "q12":
        return "\n".join(
            [
                "bash tests/verification/formalsql/scripts/compile_q12_resultspec.sh",
                "bash tests/verification/formalsql/scripts/compile_q12_normalize_sound.sh",
                "bash tests/verification/formalsql/scripts/check_q12_rocq_no_admitted.sh",
                "The normalize script must print Q12_SQL_SPEC_PROVED OK",
                "bash tests/verification/formalsql/scripts/check_print_assumptions.sh q12",
            ]
        )
    tag = f"Q{query[1:]}"
    return "\n".join(
        [
            f"There is no handwritten Rocq development for {query}. Generate it.",
            f"Write tests/verification/formalsql/scripts/compile_{query}_normalize_sound.sh",
            f"and make it print {tag}_SQL_SPEC_PROVED OK.",
            f"Write tests/verification/formalsql/{query}/proof_manifest.rocq.json with",
            "require, theorems, rocq (list of repo-relative .v paths), and preconditions",
            "(a JSON list). Do not write the dafny list; the Dafny session owns that.",
            f"Write tests/verification/formalsql/{query}/allowed_assumptions.txt.",
            f"bash tests/verification/formalsql/scripts/check_print_assumptions.sh {query}",
        ]
    )


def accel_gate_commands(query: str) -> str:
    if query == "q1":
        files = [
            "tests/verification/q1/dafny/generated/Q1AuxSpec.dfy",
            "tests/verification/q1/dafny/Q1TwinSpecBridge.dfy",
            "tests/verification/q1/dafny/Q1Aux.dfy",
            "tests/verification/q1/dafny/Q1ResultSpecBridge.dfy",
            "tests/verification/q1/dafny/Q1Maintenance.dfy",
            "tests/verification/q1/dafny/Q1RuntimeResultSpec.dfy",
            "tests/verification/q1/dafny/Q1RuntimeAuxCorrect.dfy",
        ]
        lines = [f"tools/dafny/dafny verify {path} --allow-warnings" for path in files]
        lines.append("bash scripts/run_dafny_q1_extract_diff.sh")
        return "\n".join(lines)
    if query == "q12":
        files = [
            "tests/verification/patterns/dafny/SortedUniqueMap.dfy",
            "tests/verification/q12/dafny/Q12Aux.dfy",
            "tests/verification/q12/dafny/Q12ResultSpecBridge.dfy",
            "tests/verification/q12/dafny/Q12Maintenance.dfy",
            "tests/verification/q12/dafny/Q12RuntimeResultSpec.dfy",
        ]
        return "\n".join(f"tools/dafny/dafny verify {path} --allow-warnings" for path in files)
    return "\n".join(
        [
            f"Write tests/verification/formalsql/{query}/proof_manifest.dafny.json",
            "Its dafny list must include a model file, a *Maintenance* file, and a *Runtime* file.",
            "Use repo-relative paths. Do not rewrite the Rocq fields.",
            "tools/dafny/dafny verify <each path in proof_manifest.json dafny> --allow-warnings",
            "Do not modify the C++ accelerator.",
        ]
    )


def spec_files(query: str) -> str:
    if query == "q1":
        return "\n".join(
            [
                "tests/verification/formalsql/resultspec/QueryResultSpec.v",
                "tests/verification/formalsql/resultspec/EvalSpec.v",
                "tests/verification/q1/dafny/generated/Q1ResultSpec.dfy",
                "tests/verification/q1/dafny/generated/Q1ResultSpecData.dfy",
            ]
        )
    if query == "q12":
        return "\n".join(
            [
                "tests/verification/formalsql/resultspec/QueryResultSpecQ12.v",
                "tests/verification/formalsql/resultspec/Q12EvalSpec.v",
                "tests/verification/q12/dafny/generated/Q12ResultSpec.dfy",
                "tests/verification/q12/dafny/generated/Q12ResultSpecData.dfy",
            ]
        )
    return f"tests/verification/formalsql/{query}/proof_manifest.json"


def launch_session(
    root: Path,
    *,
    query: str,
    stage: str,
    model: str,
    prompt: str,
    log_path: Path,
) -> int:
    orch = root / "pipeline" / "orchestrator"
    sys.path.insert(0, str(orch))
    from accel_pipeline.backends.base import StageRunRequest
    from accel_pipeline.backends.codex_exec import CodexExecBackend
    from accel_pipeline.config import ModelConfig, PipelineConfig

    pipeline = PipelineConfig(
        repo_root=root,
        pipeline_root=root / "pipeline",
        build_dir=root / "build-release",
        tpch_db_sf1=root / "tpch_sf1.duckdb",
        tpch_db_sf10=root / "tpch_sf10.duckdb",
    )
    models = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={},
        stages={stage: model},
    )
    backend = CodexExecBackend(
        pipeline_config=pipeline,
        model_config=models,
        query=1 if query == "q1" else 12,
        stage=stage,
        workload_name=query,
        art_dir=log_path.parent,
    )
    result = backend.run_stage(
        StageRunRequest(
            stage=stage,
            instructions="Follow the verification session contract exactly.",
            user_message=prompt,
            agent_name=stage,
            model_id=model,
            agent_model=model,
        )
    )
    log_path.write_text(result.final_text or result.error or "", encoding="utf-8")
    return 0 if result.status == "finished" else 1


def write_manifest(
    path: Path,
    *,
    query: str,
    catalog_sql: Path,
    catalog_sha: str,
    accelerator: Path,
    gates: dict[str, str],
    sessions: dict[str, str],
    library_additions: list[dict] | None = None,
) -> None:
    meta = QUERY_META[query]
    # runs/<q>/verification_manifest.json → parents[2] is tests/verification
    formalsql = path.parents[2] / "formalsql" / query
    body = {
        "query": query,
        "catalog_sql": str(catalog_sql),
        "catalog_sha256": catalog_sha,
        "formalsql": {
            "queries": str(formalsql / "generated" / "Queries.v"),
            "schema": str(formalsql / "generated" / "Schema.v"),
            "witness": str(formalsql / "generated" / "Witness.v"),
        },
        "canonical_spec": str(path.parents[2] / "specs" / query / "canonical_spec.json"),
        "spec_artifacts": spec_files(query).splitlines(),
        "accelerator": str(accelerator),
        "sessions": sessions,
        "gates": gates,
        "preconditions": meta["preconditions"],
        "tcb": meta["tcb"],
        "semantic_residuals": meta["semantic_residuals"],
        "pattern_library_additions": library_additions or [],
        "semantic_path_closed": meta["semantic_residuals"] == [] and gates.get("NO_QUERY_SEMANTIC_RESIDUAL") == "pass",
        "composed_claim": meta["claim"],
    }
    generated = load_proof_manifest(path.parents[4], query)
    if generated:
        preconditions = generated.get("preconditions")
        if isinstance(preconditions, list) and preconditions:
            body["preconditions"] = preconditions
        if isinstance(generated.get("semantic_residuals"), list):
            body["semantic_residuals"] = generated["semantic_residuals"]
            body["semantic_path_closed"] = (
                generated["semantic_residuals"] == []
                and gates.get("NO_QUERY_SEMANTIC_RESIDUAL") == "pass"
            )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(body, indent=2) + "\n", encoding="utf-8")
    summary = path.parent / "summary.md"
    lines = [
        f"# {query} verification summary",
        "",
        body["composed_claim"],
        "",
        "## Gates",
        "",
    ]
    for name, status in gates.items():
        lines.append(f"- {name}: {status}")
    lines.extend(["", "## Preconditions", ""])
    lines.extend(f"- {item}" for item in body["preconditions"])
    lines.extend(["", "## TCB", ""])
    lines.extend(f"- {item['name']} ({item['kind']})" for item in body["tcb"])
    lines.extend(["", "## Semantic residuals", ""])
    if body["semantic_residuals"]:
        lines.extend(f"- {item}" for item in body["semantic_residuals"])
    else:
        lines.append("- none")
    lines.extend(["", "## Pattern library additions", ""])
    if body["pattern_library_additions"]:
        lines.extend(f"- {item['name']} ({item['path']})" for item in body["pattern_library_additions"])
    else:
        lines.append("- none")
    lines.append("")
    summary.write_text("\n".join(lines), encoding="utf-8")


def logos_ok(query: str, root: Path, runner: GateRunner, *, skip_if_present: bool) -> tuple[bool, str]:
    generated = root / "tests/verification/formalsql" / query / "generated" / "Queries.v"
    sha_path = root / "tests/verification/formalsql" / query / "catalog_sql.sha256"
    if skip_if_present and generated.is_file() and sha_path.is_file():
        return True, sha_path.read_text(encoding="utf-8").strip()
    proc = runner.script("scripts/logos_lower_catalog.sh", query)
    if proc.returncode != 0 or not generated.is_file():
        return False, ""
    return True, sha_path.read_text(encoding="utf-8").strip()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query", required=True, choices=sorted(QUERY_META))
    parser.add_argument("--catalog-sql", type=Path)
    parser.add_argument("--accelerator", type=Path)
    parser.add_argument("--model-spec-agent")
    parser.add_argument("--model-sql-agent")
    parser.add_argument("--model-accel-agent")
    parser.add_argument("--model-critique-agent")
    parser.add_argument("--critique-rounds", type=int, default=3)
    parser.add_argument("--repair-rounds", type=int, default=5)
    parser.add_argument("--deterministic-only", action="store_true")
    return parser


def _sql_prompt(root: Path, query: str, catalog_sql: Path, spec_path: Path, run_dir: Path, critique: str, plan: Path, round_i: int, repair_rounds: int, gate_status: str) -> str:
    return fill_prompt(
        (root / "tests/verification/agent/prompts/session1_sql_spec.md").read_text(encoding="utf-8"),
        {
            "QUERY": query,
            "CATALOG_SQL": str(catalog_sql),
            "FORMALSQL_DIR": str(root / "tests/verification/formalsql" / query / "generated"),
            "SCHEMA_SQL": str(root / "tests/verification/formalsql" / query / "schema.sql"),
            "CANONICAL_SPEC": str(spec_path),
            "CRITIQUE": critique,
            "REUSE_RECORD": str(run_dir / "sql_reuse.json"),
            "PATTERN_CATALOG": str(catalog_path(root)),
            "GATE_COMMANDS": sql_gate_commands(query),
            "PLAN": str(plan),
            "PLAN_TEXT": read_plan(plan),
            "ROUND": str(round_i),
            "REPAIR_ROUNDS": str(repair_rounds),
            "GATE_STATUS": gate_status,
        },
    )


def _accel_prompt(root: Path, query: str, accelerator: Path, spec_path: Path, run_dir: Path, critique: str, plan: Path, round_i: int, repair_rounds: int, gate_status: str) -> str:
    return fill_prompt(
        (root / "tests/verification/agent/prompts/session2_accel.md").read_text(encoding="utf-8"),
        {
            "QUERY": query,
            "ACCELERATOR": str(accelerator),
            "SPEC_FILES": spec_files(query),
            "CANONICAL_SPEC": str(spec_path),
            "CRITIQUE": critique,
            "REUSE_RECORD": str(run_dir / "accel_reuse.json"),
            "PATTERN_CATALOG": str(catalog_path(root)),
            "GATE_COMMANDS": accel_gate_commands(query),
            "PLAN": str(plan),
            "PLAN_TEXT": read_plan(plan),
            "ROUND": str(round_i),
            "REPAIR_ROUNDS": str(repair_rounds),
            "GATE_STATUS": gate_status,
        },
    )


def run_sql_track(
    root: Path,
    query: str,
    *,
    catalog_sql: Path,
    spec_path: Path,
    spec_hash: str,
    run_dir: Path,
    runner: GateRunner,
    model: str,
    critique_model: str,
    repair_rounds: int,
    critique_rounds: int,
    deterministic_only: bool,
) -> dict:
    sessions: dict[str, str] = {}
    gates = sql_gates(query, runner)
    additions: list[dict] = []
    plan = run_dir / "sql_plan.md"
    critique_note = "No critique yet."
    if gates_green(gates):
        sessions["sql_spec"] = "skipped_green"
    elif deterministic_only:
        sessions["sql_spec"] = "skipped_deterministic_only"
    else:
        for round_i in range(1, repair_rounds + 1):
            log_name = "session1.log" if round_i == 1 else f"session1_repair_{round_i}.log"
            code = launch_session(
                root,
                query=query,
                stage="sql_spec",
                model=model,
                prompt=_sql_prompt(
                    root, query, catalog_sql, spec_path, run_dir, critique_note, plan, round_i, repair_rounds, format_gate_status(gates)
                ),
                log_path=run_dir / log_name,
            )
            sessions["sql_spec"] = "finished" if code == 0 else "error"
            sessions["sql_repair_round"] = str(round_i)
            if sha256_file(spec_path) != spec_hash:
                gates["CANONICAL_SPEC_OK"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "canonical spec changed during the Rocq session"}
            gates = sql_gates(query, runner)
            if gates_green(gates):
                break
            if round_i < repair_rounds:
                print(f"verify_query: Rocq round {round_i} left gates red; starting another round", file=sys.stderr)
        else:
            print("verify_query: Rocq repair rounds exhausted", file=sys.stderr)
    if sessions.get("sql_spec") == "finished" and gates_green(gates):
        added, err = register_reuse(root, query, "sql_spec", run_dir / "sql_reuse.json")
        if err:
            gates["PATTERN_LIBRARY_UPDATED"] = "fail"
            return {"sessions": sessions, "gates": gates, "additions": additions, "error": err}
        additions.extend(added)
    if not deterministic_only and gates_green(gates) and sessions.get("sql_spec") == "finished":
        for round_i in range(1, critique_rounds + 1):
            report = run_dir / f"sql_critique_{round_i}.json"
            prompt = fill_prompt(
                (root / "tests/verification/agent/prompts/session_critique.md").read_text(encoding="utf-8"),
                {
                    "QUERY": query,
                    "STAGE": "sql_spec",
                    "CANONICAL_SPEC": str(spec_path),
                    "CRITIQUE_REPORT": str(report),
                },
            )
            code = launch_session(
                root,
                query=query,
                stage="critique_sql",
                model=critique_model,
                prompt=prompt,
                log_path=run_dir / f"sql_critique_{round_i}.log",
            )
            sessions[f"sql_critique_{round_i}"] = "finished" if code == 0 else "error"
            verdict, feedback = load_critique(report)
            if verdict == "accept":
                gates["CRITIQUE_SQL"] = "pass"
                break
            if round_i == critique_rounds:
                gates["CRITIQUE_SQL"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "SQL critique still requires revision"}
            critique_note = feedback
            code = launch_session(
                root,
                query=query,
                stage="sql_spec",
                model=model,
                prompt=_sql_prompt(
                    root, query, catalog_sql, spec_path, run_dir, critique_note, plan, round_i + 1, repair_rounds, format_gate_status(gates)
                ),
                log_path=run_dir / f"session1_round_{round_i + 1}.log",
            )
            sessions["sql_spec"] = "finished" if code == 0 else "error"
            if sha256_file(spec_path) != spec_hash:
                gates["CANONICAL_SPEC_OK"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "canonical spec changed during the Rocq session"}
            gates = sql_gates(query, runner)
            if not gates_green(gates):
                gates["CRITIQUE_SQL"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "Rocq gates failed after critique"}
    elif deterministic_only:
        gates["CRITIQUE_SQL"] = "skipped"
    return {"sessions": sessions, "gates": gates, "additions": additions, "error": None}


def run_accel_track(
    root: Path,
    query: str,
    *,
    accelerator: Path,
    spec_path: Path,
    spec_hash: str,
    run_dir: Path,
    runner: GateRunner,
    model: str,
    critique_model: str,
    repair_rounds: int,
    critique_rounds: int,
    deterministic_only: bool,
) -> dict:
    sessions: dict[str, str] = {}
    gates = accel_gates(query, runner)
    additions: list[dict] = []
    plan = run_dir / "accel_plan.md"
    critique_note = "No critique yet."
    if gates_green(gates):
        sessions["accel_verify"] = "skipped_green"
    elif deterministic_only:
        sessions["accel_verify"] = "skipped_deterministic_only"
    else:
        for round_i in range(1, repair_rounds + 1):
            before = sha256_tree(accelerator)
            log_name = "session2.log" if round_i == 1 else f"session2_repair_{round_i}.log"
            code = launch_session(
                root,
                query=query,
                stage="accel_verify",
                model=model,
                prompt=_accel_prompt(
                    root, query, accelerator, spec_path, run_dir, critique_note, plan, round_i, repair_rounds, format_gate_status(gates)
                ),
                log_path=run_dir / log_name,
            )
            sessions["accel_verify"] = "finished" if code == 0 else "error"
            sessions["accel_repair_round"] = str(round_i)
            if sha256_file(spec_path) != spec_hash:
                gates["CANONICAL_SPEC_OK"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "canonical spec changed during the Dafny session"}
            if sha256_tree(accelerator) != before:
                gates["CPP_MODEL_CORRESPONDENCE"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "accelerator directory changed during the Dafny session"}
            gates = accel_gates(query, runner)
            if gates_green(gates):
                break
            if round_i < repair_rounds:
                print(f"verify_query: Dafny round {round_i} left gates red; starting another round", file=sys.stderr)
        else:
            print("verify_query: Dafny repair rounds exhausted", file=sys.stderr)
    if sessions.get("accel_verify") == "finished" and gates_green(gates):
        added, err = register_reuse(root, query, "accel_verify", run_dir / "accel_reuse.json")
        if err:
            gates["PATTERN_LIBRARY_UPDATED"] = "fail"
            return {"sessions": sessions, "gates": gates, "additions": additions, "error": err}
        additions.extend(added)
    if not deterministic_only and gates_green(gates) and sessions.get("accel_verify") == "finished":
        for round_i in range(1, critique_rounds + 1):
            report = run_dir / f"accel_critique_{round_i}.json"
            prompt = fill_prompt(
                (root / "tests/verification/agent/prompts/session_critique.md").read_text(encoding="utf-8"),
                {
                    "QUERY": query,
                    "STAGE": "accel_verify",
                    "CANONICAL_SPEC": str(spec_path),
                    "CRITIQUE_REPORT": str(report),
                },
            )
            code = launch_session(
                root,
                query=query,
                stage="critique_accel",
                model=critique_model,
                prompt=prompt,
                log_path=run_dir / f"accel_critique_{round_i}.log",
            )
            sessions[f"accel_critique_{round_i}"] = "finished" if code == 0 else "error"
            verdict, feedback = load_critique(report)
            if verdict == "accept":
                gates["CRITIQUE_ACCEL"] = "pass"
                break
            if round_i == critique_rounds:
                gates["CRITIQUE_ACCEL"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "accelerator critique still requires revision"}
            critique_note = feedback
            before = sha256_tree(accelerator)
            code = launch_session(
                root,
                query=query,
                stage="accel_verify",
                model=model,
                prompt=_accel_prompt(
                    root, query, accelerator, spec_path, run_dir, critique_note, plan, round_i + 1, repair_rounds, format_gate_status(gates)
                ),
                log_path=run_dir / f"session2_round_{round_i + 1}.log",
            )
            sessions["accel_verify"] = "finished" if code == 0 else "error"
            if sha256_file(spec_path) != spec_hash or sha256_tree(accelerator) != before:
                gates["CRITIQUE_ACCEL"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "spec or accelerator changed during the Dafny session"}
            gates = accel_gates(query, runner)
            if not gates_green(gates):
                gates["CRITIQUE_ACCEL"] = "fail"
                return {"sessions": sessions, "gates": gates, "additions": additions, "error": "Dafny gates failed after critique"}
    elif deterministic_only:
        gates["CRITIQUE_ACCEL"] = "skipped"
    return {"sessions": sessions, "gates": gates, "additions": additions, "error": None}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = repo_root()
    load_repo_env(root)
    query = args.query
    meta = QUERY_META[query]
    accelerator = args.accelerator or (root / meta["accel_rel"])
    if not args.deterministic_only and (
        not args.model_spec_agent
        or not args.model_sql_agent
        or not args.model_accel_agent
        or not args.model_critique_agent
    ):
        print(
            "verify_query: --model-spec-agent, --model-sql-agent, "
            "--model-accel-agent, and --model-critique-agent are required",
            file=sys.stderr,
        )
        return 2
    if args.critique_rounds < 1 or args.repair_rounds < 1:
        print("verify_query: --critique-rounds and --repair-rounds must be at least 1", file=sys.stderr)
        return 2

    run_dir = root / "tests/verification/runs" / query
    run_dir.mkdir(parents=True, exist_ok=True)
    runner = GateRunner(root)
    sessions = {"canonical_spec": "not_run", "sql_spec": "not_run", "accel_verify": "not_run"}
    library_additions: list[dict] = []
    gates: dict[str, str] = {}

    lowered, catalog_sha = logos_ok(query, root, runner, skip_if_present=args.deterministic_only)
    gates["LOGOS_OK"] = "pass" if lowered else "fail"
    catalog_sql = root / "tests/verification/formalsql" / query / "catalog_base_sql.sql"
    if args.catalog_sql is not None:
        given = sha256_file(args.catalog_sql)
        if given != catalog_sha:
            print("verify_query: --catalog-sql SHA-256 does not match Logos extract", file=sys.stderr)
            gates["LOGOS_OK"] = "fail"
    if gates["LOGOS_OK"] != "pass":
        write_manifest(
            run_dir / "verification_manifest.json",
            query=query,
            catalog_sql=catalog_sql,
            catalog_sha=catalog_sha,
            accelerator=accelerator,
            gates=gates,
            sessions=sessions,
        )
        return 1

    spec_path = canonical_spec_path(root, query)
    gates["CANONICAL_SPEC_OK"] = canonical_spec_status(spec_path, query)
    if gates["CANONICAL_SPEC_OK"] != "pass" and not args.deterministic_only:
        prompt = fill_prompt(
            (root / "tests/verification/agent/prompts/session0_canonical_spec.md").read_text(encoding="utf-8"),
            {
                "CATALOG_SQL": str(catalog_sql),
                "SCHEMA_SQL": str(root / "tests/verification/formalsql" / query / "schema.sql"),
                "FORMALSQL_DIR": str(root / "tests/verification/formalsql" / query / "generated"),
                "CANONICAL_SPEC": str(spec_path),
            },
        )
        code = launch_session(
            root,
            query=query,
            stage="canonical_spec",
            model=args.model_spec_agent,
            prompt=prompt,
            log_path=run_dir / "session0.log",
        )
        sessions["canonical_spec"] = "finished" if code == 0 else "error"
        gates["CANONICAL_SPEC_OK"] = canonical_spec_status(spec_path, query)
    elif gates["CANONICAL_SPEC_OK"] == "pass":
        sessions["canonical_spec"] = "skipped_green"
    else:
        sessions["canonical_spec"] = "skipped_deterministic_only"
    if gates["CANONICAL_SPEC_OK"] != "pass":
        print(
            f"verify_query: canonical spec at {spec_path} is missing or incomplete",
            file=sys.stderr,
        )
        write_manifest(
            run_dir / "verification_manifest.json",
            query=query,
            catalog_sql=catalog_sql,
            catalog_sha=catalog_sha,
            accelerator=accelerator,
            gates=gates,
            sessions=sessions,
        )
        return 1
    spec_hash = sha256_file(spec_path)
    track_kwargs = dict(
        spec_path=spec_path,
        spec_hash=spec_hash,
        run_dir=run_dir,
        runner=runner,
        repair_rounds=args.repair_rounds,
        critique_rounds=args.critique_rounds,
        deterministic_only=args.deterministic_only,
    )
    with ThreadPoolExecutor(max_workers=2) as pool:
        sql_future = pool.submit(
            run_sql_track,
            root,
            query,
            catalog_sql=catalog_sql,
            model=args.model_sql_agent or "",
            critique_model=args.model_critique_agent or "",
            **track_kwargs,
        )
        accel_future = pool.submit(
            run_accel_track,
            root,
            query,
            accelerator=accelerator,
            model=args.model_accel_agent or "",
            critique_model=args.model_critique_agent or "",
            **track_kwargs,
        )
        sql_result = sql_future.result()
        accel_result = accel_future.result()
    for result in (sql_result, accel_result):
        sessions.update(result["sessions"])
        gates.update(result["gates"])
        library_additions.extend(result["additions"])
        if result["error"]:
            print(f"verify_query: {result['error']}", file=sys.stderr)
    if gates_green(gates):
        gates["PATTERN_LIBRARY_UPDATED"] = gates.get("PATTERN_LIBRARY_UPDATED", "pass")
    write_manifest(
        run_dir / "verification_manifest.json",
        query=query,
        catalog_sql=catalog_sql,
        catalog_sha=catalog_sha,
        accelerator=accelerator,
        gates=gates,
        sessions=sessions,
        library_additions=library_additions,
    )
    return 0 if gates_green(gates) else 1


if __name__ == "__main__":
    sys.exit(main())
