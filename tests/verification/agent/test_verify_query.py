"""Unit tests for the verification driver. Gates are stubbed; Rocq and Dafny are not run."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import verify_query as vq


def _proc(code: int = 0, stdout: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=[], returncode=code, stdout=stdout, stderr="")


def test_print_assumptions_closed_and_axioms():
    assert vq.parse_print_assumptions("Closed under the global context\n") == set()
    text = "Axioms:\nClassical_Prop.classic : forall P, P \\/ ~ P\n\n"
    assert vq.parse_print_assumptions(text) == {"Classical_Prop.classic"}
    assert vq.parse_print_assumptions("not a print-assumptions dump") is None


def test_allowlist_ignores_comments():
    path = Path(tempfile.mkdtemp()) / "allowed_assumptions.txt"
    path.write_text("# comment\n\nFoo.bar\n")
    assert vq.allowlist_names(path) == {"Foo.bar"}


def test_q12_sql_gates_require_proved_line():
    calls: list[list[str]] = []

    def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
        calls.append(cmd)
        joined = " ".join(cmd)
        if "compile_q12_normalize_sound" in joined:
            return _proc(0, "Q12_SQL_SPEC_PROVED OK\nNORMALIZE_SOUND_Q12 OK\n")
        return _proc(0, "OK\n")

    status = vq.sql_gates("q12", vq.GateRunner(ROOT, run=run))
    assert status["SQL_SPEC_PROVED"] == "pass"
    assert status["NO_QUERY_SEMANTIC_RESIDUAL"] == "pass"
    assert status["NO_ADMITTED"] == "pass"
    assert status["PRINT_ASSUMPTIONS"] == "pass"
    assert status["SPEC_EXPORT_CHECKED"] == "pass"
    assert vq.gates_green(status)


def test_q12_residual_scanner_fails_closed():
    def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
        joined = " ".join(cmd)
        if "compile_q12_normalize_sound" in joined:
            return _proc(0, "Q12_SQL_SPEC_PROVED pending\nNORMALIZE_SOUND_Q12 OK\n")
        return _proc(0, "OK\n")

    status = vq.sql_gates("q12", vq.GateRunner(ROOT, run=run))
    assert status["SQL_SPEC_PROVED"] == "fail"
    assert status["NO_QUERY_SEMANTIC_RESIDUAL"] == "fail"
    assert not vq.gates_green(status)


def test_manifest_splits_preconditions_tcb_and_residuals():
    tmp_path = Path(tempfile.mkdtemp())
    manifest = tmp_path / "tests" / "verification" / "runs" / "q12" / "verification_manifest.json"
    vq.write_manifest(
        manifest,
        query="q12",
        catalog_sql=Path("catalog.sql"),
        catalog_sha="abc",
        accelerator=Path("aux"),
        gates={"NO_QUERY_SEMANTIC_RESIDUAL": "pass", "LOGOS_OK": "pass"},
        sessions={"sql_spec": "skipped_green", "accel_verify": "skipped_green"},
    )
    body = json.loads(manifest.read_text())
    assert "q12_sum_bound orders lines" in body["preconditions"]
    assert body["semantic_residuals"] == []
    assert body["semantic_path_closed"] is True
    kinds = {item["kind"] for item in body["tcb"]}
    assert "numeric_representation" not in kinds
    assert "cpp_model" in kinds
    assert "none" in (manifest.parent / "summary.md").read_text()

    q1 = tmp_path / "tests" / "verification" / "runs" / "q1" / "verification_manifest.json"
    vq.write_manifest(
        q1,
        query="q1",
        catalog_sql=Path("catalog.sql"),
        catalog_sha="def",
        accelerator=Path("aux"),
        gates={"NO_QUERY_SEMANTIC_RESIDUAL": "pass"},
        sessions={"sql_spec": "skipped_green", "accel_verify": "skipped_green"},
    )
    q1_body = json.loads(q1.read_text())
    assert "Q1NumericSafe db" in q1_body["preconditions"]
    assert any(item["name"] == "NumericEmbedFaithful" and item["kind"] == "numeric_representation" for item in q1_body["tcb"])
    assert q1_body["semantic_residuals"] == []


def test_cpp_hash_detects_accelerator_edits():
    aux = Path(tempfile.mkdtemp()) / "aux"
    aux.mkdir()
    target = aux / "accel.cpp"
    target.write_text("int x;\n")
    before = vq.sha256_tree(aux)
    target.write_text("int y;\n")
    assert vq.sha256_tree(aux) != before


def test_maintenance_axiom_fails():
    root = Path(tempfile.mkdtemp())
    maint = root / "tests/verification/q12/dafny"
    maint.mkdir(parents=True)
    (maint / "Q12Maintenance.dfy").write_text("lemma L() { assume {:axiom} true; }\n")
    (maint / "Q12Aux.dfy").write_text("module Q12Aux {}\n")
    (root / "tests/verification/q12/dafny").mkdir(parents=True, exist_ok=True)
    (root / "tests/verification/patterns/dafny").mkdir(parents=True)

    def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
        return _proc(0, "verified")

    status = vq.accel_gates("q12", vq.GateRunner(root, run=run))
    assert status["MAINTENANCE_PROVED"] == "fail"
    assert status["CPP_MODEL_CORRESPONDENCE"] == "unproved"


def test_cli_requires_models():
    assert vq.main(["--query", "q12"]) == 2


def test_seeded_canonical_specs_pass():
    root = vq.repo_root()
    for query in ("q1", "q12"):
        path = vq.canonical_spec_path(root, query)
        assert vq.canonical_spec_status(path, query) == "pass"


def test_canonical_spec_accepts_sql_text_as_query_field():
    path = Path(tempfile.mkdtemp()) / "canonical_spec.json"
    path.write_text(
        json.dumps(
            {
                "query": "SELECT promo_revenue FROM lineitem, part",
                "inputs": [],
                "parameters": {},
                "join": None,
                "filter": "true",
                "group_key": [],
                "contributions": {},
                "aggregates": [],
                "final_projection": [],
                "ordering": [],
            }
        )
    )
    assert vq.canonical_spec_status(path, "q14") == "pass"
    path = Path(tempfile.mkdtemp()) / "canonical_spec.json"
    path.write_text('{"query": "q12", "inputs": []}\n')
    assert vq.canonical_spec_status(path, "q12") == "fail"


def test_register_reuse_adds_only_library_lemmas():
    root = Path(tempfile.mkdtemp())
    lib = root / "tests/verification/formalsql/lib"
    query_dir = root / "tests/verification/formalsql/q9"
    catalog_dir = root / "tests/verification/patterns"
    lib.mkdir(parents=True)
    query_dir.mkdir(parents=True)
    catalog_dir.mkdir(parents=True)
    (catalog_dir / "CATALOG.json").write_text('{"entries": []}\n')
    (lib / "Shared.v").write_text("Lemma shared : True. Proof. exact I. Qed.\n")
    (query_dir / "Leaf.v").write_text("Lemma leaf : True. Proof. exact I. Qed.\n")
    record = root / "sql_reuse.json"
    record.write_text(
        json.dumps(
            {
                "query": "q12",
                "session": "sql_spec",
                "reused": [],
                "additions": [
                    {
                        "name": "shared",
                        "kind": "reusable",
                        "path": "tests/verification/formalsql/lib/Shared.v",
                        "generalizes": "a fact later queries can call",
                    },
                    {
                        "name": "leaf",
                        "kind": "query_leaf",
                        "path": "tests/verification/formalsql/q9/Leaf.v",
                    },
                ],
            }
        )
    )
    added, err = vq.register_reuse(root, "q12", "sql_spec", record)
    assert err is None
    assert [item["name"] for item in added] == ["shared"]
    stored = json.loads((catalog_dir / "CATALOG.json").read_text())
    assert [item["path"] for item in stored["entries"]] == ["tests/verification/formalsql/lib/Shared.v"]
    added_again, err_again = vq.register_reuse(root, "q12", "sql_spec", record)
    assert err_again is None
    assert added_again == []


def test_register_reuse_rejects_reusable_query_file():
    root = Path(tempfile.mkdtemp())
    query_dir = root / "tests/verification/formalsql/q9"
    catalog_dir = root / "tests/verification/patterns"
    query_dir.mkdir(parents=True)
    catalog_dir.mkdir(parents=True)
    (catalog_dir / "CATALOG.json").write_text('{"entries": []}\n')
    (query_dir / "Leaf.v").write_text("Lemma leaf : True.\n")
    record = root / "sql_reuse.json"
    record.write_text(
        json.dumps(
            {
                "query": "q12",
                "session": "sql_spec",
                "additions": [
                    {
                        "name": "leaf",
                        "kind": "reusable",
                        "path": "tests/verification/formalsql/q9/Leaf.v",
                    }
                ],
            }
        )
    )
    added, err = vq.register_reuse(root, "q12", "sql_spec", record)
    assert added == []
    assert err is not None
    assert "outside the pattern library" in err


def test_q14_is_an_entry_and_has_no_seeded_proof():
    args = vq.build_parser().parse_args(["--query", "q14", "--deterministic-only"])
    assert args.query == "q14"
    assert vq.canonical_spec_status(vq.canonical_spec_path(vq.repo_root(), "q14"), "q14") == "pass"
    calls: list[list[str]] = []

    def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
        calls.append(cmd)
        return _proc(1, "")

    status = vq.sql_gates("q14", vq.GateRunner(ROOT, run=run))
    assert all(value == "fail" for value in status.values())
    assert not any("q12" in " ".join(cmd) for cmd in calls)


def test_critique_accepts_only_an_empty_finding_list():
    path = Path(tempfile.mkdtemp()) / "critique.json"
    path.write_text(json.dumps({"verdict": "accept", "findings": [], "feedback": ""}) + "\n")
    assert vq.load_critique(path) == ("accept", "")
    path.write_text(json.dumps({"verdict": "accept", "findings": ["Axiom in Q14Aux.dfy"]}) + "\n")
    verdict, feedback = vq.load_critique(path)
    assert verdict == "revise"
    assert "Axiom" in feedback
    path.write_text(json.dumps({"verdict": "revise", "findings": ["promo ratio was moved into TCB"], "feedback": "prove the ratio"}) + "\n")
    verdict, feedback = vq.load_critique(path)
    assert verdict == "revise"
    assert "prove the ratio" in feedback


def test_repair_rounds_default_and_manifest_merge():
    args = vq.build_parser().parse_args(["--query", "q14", "--deterministic-only"])
    assert args.repair_rounds == 5
    root = Path(tempfile.mkdtemp())
    qdir = root / "tests/verification/formalsql/q14"
    qdir.mkdir(parents=True)
    (qdir / "proof_manifest.rocq.json").write_text(
        json.dumps({"rocq": ["tests/verification/formalsql/q14/A.v"], "preconditions": ["Domain"]}) + "\n"
    )
    (qdir / "proof_manifest.dafny.json").write_text(
        json.dumps({"dafny": ["tests/verification/q14/dafny/Q14Maintenance.dfy"]}) + "\n"
    )
    vq.merge_proof_manifests(root, "q14")
    body = json.loads((qdir / "proof_manifest.json").read_text())
    assert body["rocq"] == ["tests/verification/formalsql/q14/A.v"]
    assert body["dafny"] == ["tests/verification/q14/dafny/Q14Maintenance.dfy"]
    assert body["preconditions"] == ["Domain"]
    assert "red" in vq.format_gate_status({"SQL_SPEC_PROVED": "fail", "NO_ADMITTED": "pass"})


def test_repo_env_loads_without_overriding(monkeypatch=None):
    root = Path(tempfile.mkdtemp())
    (root / ".env").write_text("# comment\nOPENROUTER_API_KEY=from-file\nALREADY=from-file\n")
    os.environ["ALREADY"] = "from-shell"
    os.environ.pop("OPENROUTER_API_KEY", None)
    vq.load_repo_env(root)
    assert os.environ["OPENROUTER_API_KEY"] == "from-file"
    assert os.environ["ALREADY"] == "from-shell"
    os.environ.pop("OPENROUTER_API_KEY", None)
    os.environ.pop("ALREADY", None)


if __name__ == "__main__":
    test_print_assumptions_closed_and_axioms()
    test_allowlist_ignores_comments()
    test_q12_sql_gates_require_proved_line()
    test_q12_residual_scanner_fails_closed()
    test_manifest_splits_preconditions_tcb_and_residuals()
    test_cpp_hash_detects_accelerator_edits()
    test_maintenance_axiom_fails()
    test_cli_requires_models()
    test_seeded_canonical_specs_pass()
    test_canonical_spec_accepts_sql_text_as_query_field()
    test_register_reuse_adds_only_library_lemmas()
    test_register_reuse_rejects_reusable_query_file()
    test_q14_is_an_entry_and_has_no_seeded_proof()
    test_repair_rounds_default_and_manifest_merge()
    test_critique_accepts_only_an_empty_finding_list()
    test_repo_env_loads_without_overriding()
    print("test_verify_query OK")
