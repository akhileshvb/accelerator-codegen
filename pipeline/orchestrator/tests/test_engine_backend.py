"""Smoke tests for engine_backend workloads."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.config import (
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    load_model_config,
    sanitize_engine_name,
)
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.prompts import render_prompt
from accel_pipeline.stages.engine_backend import (
    implementation_layers,
    run_engine_pipeline,
)
from accel_pipeline.write_ownership import write_allowed


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_load_engine_mysql_workload():
    w = load_workload("engine_mysql", REPO_ROOT)
    assert w["workload_kind"] == "engine_backend"
    assert w["engine_name"] == "mysql"
    assert w["prompt_subdir"] == "engine_backend"
    assert w["target_paths"]["state_store_header"].endswith(
        "stateful/mysql/mysql_state_store.hpp"
    )
    assert w["target_paths"]["aux_backend_header"].endswith(
        "stateful/mysql/mysql_aux_backend.hpp"
    )
    ids = [layer["id"] for layer in w["implementation_layers"]]
    assert ids == [
        "store",
        "aux_backend",
        "txn",
        "mutations",
        "scan",
        "runtime",
        "visibility",
        "host",
    ]
    store = w["implementation_layers"][0]
    assert store["harness"] == "backends/mysql/tests/test_mysql_store.cpp"
    assert store["cmake_target"] == "test_mysql_store"
    assert store["gate"] == "engine_store"
    assert store["rules"] == ["store_roundtrip"]
    aux = next(layer for layer in w["implementation_layers"] if layer["id"] == "aux_backend")
    assert aux["rules"] == ["aux_services"]
    runtime = next(layer for layer in w["implementation_layers"] if layer["id"] == "runtime")
    assert runtime["rules"] == ["toy_runtime"]
    assert w["target_paths"]["adversarial_test"] == (
        "backends/mysql/tests/test_mysql_adversarial.cpp"
    )
    assert w["cmake"]["collect_fn"] == "accel_collect_mysql_sources"
    assert w["cmake"]["build_flag"] == "ACCELERATOR_CODEGEN_BUILD_MYSQL"
    assert "backends/mysql/**" in w["write_paths"]
    assert not any(p.startswith(("substrate", "accelerators", "workloads")) for p in w["write_paths"])
    assert "engine_store" in w["post_validation_gates"]
    assert "engine_adversarial" in w["post_validation_gates"]
    assert "engine_adversarial" not in w["post_implementation_gates"]
    assert w.get("requires_in_process_host") is True
    assert w.get("host_runtime", {}).get("kind") == "docker"
    assert w.get("host_runtime", {}).get("discovery") == "mysql_docker"
    assert w.get("host_runtime", {}).get("ensure_script") == "scripts/ensure_accel_mysql_test.sh"
    assert w.get("host_runtime", {}).get("build_script") == "scripts/build_accel_mysql_plugin.sh"
    assert w.get("host_runtime", {}).get("env", {}).get("ACCEL_MYSQL_ENSURE_BUILD") == "0"
    assert w["layer_critique_max_rounds"] == 2
    assert w["layer_gate_max_rounds"] == 3


def test_engine_artifacts_dir():
    w = load_workload("engine_mysql", REPO_ROOT)
    art = query_artifacts_dir(REPO_ROOT, 0, workload=w)
    assert "engines" in str(art)
    assert "mysql" in str(art)


def test_engine_write_paths_allow_adapter_edits():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/mysql/engine/scan_helper.hpp",
        query=0,
        stage="impl_scan",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok, reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "backends/mysql/state/mysql_aux_backend.cpp",
        query=0,
        stage="impl_aux_backend",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2
    ok3, reason3 = write_allowed(
        REPO_ROOT,
        "backends/mysql/tests/test_mysql_store.cpp",
        query=0,
        stage="impl_store",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok3, reason3
    ok4, reason4 = write_allowed(
        REPO_ROOT,
        "backends/mysql/api/catalog/query_catalog.hpp",
        query=0,
        stage="impl_runtime",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok4, reason4
    # another engine's backend, the substrate and query directories stay closed
    for rel in ("backends/duckdb/api/register.cpp", "substrate/aux/core/aux_types.hpp",
                "accelerators/tpch/q6/mysql/aux/module.cpp", "workloads/tpch/q6.sql"):
        ok5, reason5 = write_allowed(
            REPO_ROOT, rel, query=0, stage="impl_runtime", write_paths=merged.write_paths + ["**"],
            pipeline_config=merged, benchmark="engine",
        )
        assert not ok5, (rel, reason5)


def test_critique_store_cannot_write_adapter():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/postgres/state/postgres_state_store.cpp",
        query=0,
        stage="critique_store",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert not ok
    assert "pipeline/artifacts" in reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "pipeline/artifacts/engines/postgres/layer_critique_store.json",
        query=0,
        stage="critique_store",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2


def test_sanitize_engine_name():
    assert sanitize_engine_name("MySQL") == "mysql"
    assert sanitize_engine_name("my-sql") == "my_sql"


def test_render_engine_analysis_prompt():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    text = render_prompt(pc, "01_analysis.md", w, 0, "analysis")
    assert "layers.md" in text or "sequential" in text.lower()
    assert "mysql" in text.lower()
    assert "store" in text.lower()
    assert "from-scratch" in text.lower() or "do **not** read" in text.lower() or "do not read" in text.lower()
    assert "{{layer_id}}" not in text
    assert "request_gate" in text
    assert "roadmap" in text.lower() or "concrete" in text.lower()
    assert "layer_contracts.md" in text
    assert "TryPublishPartition" in text


def test_render_impl_store_prompt():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    layer = implementation_layers(w)[0]
    text = render_prompt(
        pc,
        "02_implementation.md",
        w,
        0,
        "impl_store",
        extra_context={
            "layer_id": layer["id"],
            "layer_title": layer["title"],
            "layer_gate": layer["gate"],
            "layer_harness": layer["harness"],
            "layer_cmake_target": layer["cmake_target"],
            "prior_layers": "- (this is the first layer)",
        },
    )
    assert "engine_store" in text
    assert "test_mysql_store.cpp" in text
    assert "{{layer_id}}" not in text
    assert "request_gate" in text
    assert "run_engine_build" in text
    assert "run_layer_gate" in text
    assert "engine_build" in text
    assert "later layer" in text.lower()
    assert "smallest" in text.lower() or "minimal" in text.lower()
    assert "TryPublishPartition" in text
    assert "RegisterQueryKey" in text
    assert "layer_contracts.md" in text
    assert "partitioned_state_store.hpp" in text
    assert "Native base-table" in text or "hard_guidelines" in text.lower()
    assert "Engine backend layer workflow" in text or "layer_contracts.md" in text


def test_impl_user_message_names_mcp_gate_calls():
    from accel_pipeline.stages.engine_backend import _user_message

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    layer = next(L for L in implementation_layers(w) if L["id"] == "txn")
    text = _user_message(pc, w, "impl_txn", layer=layer)
    assert 'request_gate(gate_name="engine_build")' in text
    assert 'request_gate(gate_name="engine_txn")' in text
    assert "run_engine_build()" in text
    assert "run_layer_gate()" in text
    assert "list_mcp_resources" in text


def test_postgres_impl_prompt_uses_docker_gates():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    layer = implementation_layers(w)[0]
    text = render_prompt(
        pc,
        "02_implementation.md",
        w,
        0,
        "impl_store",
        extra_context={
            "layer_id": layer["id"],
            "layer_title": layer["title"],
            "layer_gate": layer["gate"],
            "layer_harness": layer["harness"],
            "layer_cmake_target": layer["cmake_target"],
            "prior_layers": "- (this is the first layer)",
            "host_runtime_kind": "docker",
            "host_ensure_script": "scripts/ensure_accel_pg_test.sh",
            "host_build_script": "scripts/build_accel_postgres_extension.sh",
        },
    )
    assert "docker" in text.lower()
    assert "request_gate" in text
    assert "engine_store" in text
    assert "ensure_accel_pg_test.sh" in text
    assert "build-release" in text  # mentioned as what NOT to use as the bar
    assert "Do not" in text or "do not" in text.lower()


def test_postgres_prompt_includes_pipeline_discovered_sdk():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    text = render_prompt(pc, "01_analysis.md", w, 0, "analysis")
    assert "Host SDK discovery" in text or "host_runtime" in text.lower() or "docker" in text.lower()
    low = text.lower()
    assert "docker" in low or "accel_pg_test" in low or "ensure_accel_pg_test" in low
    assert w.get("host_runtime", {}).get("kind") == "docker"
    assert w.get("host_runtime", {}).get("env", {}).get("ACCEL_PG_ENSURE_BUILD") == "0"
    assert w.get("requires_in_process_host") is True


def test_mysql_prompt_includes_pipeline_discovered_sdk():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    text = render_prompt(pc, "01_analysis.md", w, 0, "analysis")
    low = text.lower()
    assert "docker" in low or "accel_mysql_test" in low or "ensure_accel_mysql_test" in low
    assert w.get("host_runtime", {}).get("kind") == "docker"
    assert w.get("host_runtime", {}).get("env", {}).get("ACCEL_MYSQL_ENSURE_BUILD") == "0"
    assert w.get("requires_in_process_host") is True
    assert "accel_pg_test" not in low


def test_apply_mysql_docker_env_stamps_connection():
    from accel_pipeline.mysql_docker import apply_mysql_docker_env

    env: dict[str, str] = {}
    host = apply_mysql_docker_env(env)
    assert env["ACCEL_MYSQL_DOCKER"] == "1"
    assert env["ACCEL_MYSQL_CONTAINER"] == host.container
    assert env["MYSQL_HOST"] == "127.0.0.1"
    assert env["MYSQL_TCP_PORT"] == "3307"
    assert host.plugin_so.endswith("accelerator_codegen_mysql.so")


def test_postgres_pipeline_writes_host_sdk_discovery(tmp_path, monkeypatch):
    from accel_pipeline.stages.engine_backend import _prepare_engine_host_sdk

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    _prepare_engine_host_sdk(pc, w, 0)
    art = query_artifacts_dir(REPO_ROOT, 0, workload=w)
    path = art / "host_sdk_discovery.json"
    assert path.is_file()
    data = json.loads(path.read_text(encoding="utf-8"))
    assert data["engine_name"] == "postgres"
    assert "found" in data
    assert data.get("mode") == "docker" or "docker" in str(data.get("summary") or "").lower()


def test_engine_pipeline_dry_run():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="all", skip_gates=True)


def test_engine_pipeline_dry_run_impl_store():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(
        pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="impl_store", skip_gates=True
    )


def test_host_layer_skipped_without_in_process_host():
    from accel_pipeline.stages.engine_backend import _layer_should_run

    w = load_workload("engine_mysql", REPO_ROOT)
    host = next(layer for layer in w["implementation_layers"] if layer["id"] == "host")
    store = next(layer for layer in w["implementation_layers"] if layer["id"] == "store")
    assert _layer_should_run(w, store)
    assert _layer_should_run(w, host)
    skipped = dict(w)
    skipped["requires_in_process_host"] = False
    assert not _layer_should_run(skipped, host)
    pg = load_workload("engine_postgres", REPO_ROOT)
    pg_host = next(layer for layer in pg["implementation_layers"] if layer["id"] == "host")
    assert _layer_should_run(pg, pg_host)


def test_mysql_analysis_prompt_does_not_inject_other_engine_host():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    text = render_prompt(pc, "01_analysis.md", w, 0, "analysis")
    low = text.lower()
    assert "accel_pg_test" not in low
    assert "postgres:17" not in low
    assert "accel_mysql_test" in low or "mysql:8.4" in low or "docker" in low
    assert "layers.md" in text or "sequential" in low
    assert "{{layer_id}}" not in text


def test_audit_restarts_from_earliest_failing_layer():
    from accel_pipeline.stages.engine_architecture_audit import (
        earliest_failing_layer_id,
        failing_layer_ids,
    )

    w = load_workload("engine_mysql", REPO_ROOT)
    audit = {
        "violations": [
            {"rule": "visibility", "layer": "visibility"},
            {"rule": "mutation_hooks", "layer": "mutations"},
            {"rule": "visibility", "layer": "visibility"},
        ]
    }
    assert failing_layer_ids(audit, w) == ["mutations", "visibility"]
    assert earliest_failing_layer_id(audit, w) == "mutations"
    assert earliest_failing_layer_id({"violations": []}, w) is None
    assert earliest_failing_layer_id({"violations": [{"rule": "fail_closed"}]}, w) is None
    assert failing_layer_ids({"violations": [{"rule": "fail_closed"}]}, w) == []


def test_discover_host_sdk_follows_workload_plugin_not_engine_name():
    from accel_pipeline.host_runtime import discover_host_sdk_json

    mysql = json.loads(discover_host_sdk_json(load_workload("engine_mysql", REPO_ROOT)))
    assert mysql.get("kind") == "docker" or mysql.get("mode") == "docker"
    assert mysql.get("found") is True
    postgres = json.loads(discover_host_sdk_json(load_workload("engine_postgres", REPO_ROOT)))
    assert postgres.get("kind") == "docker" or postgres.get("mode") == "docker"
    none = json.loads(discover_host_sdk_json({"engine_name": "other"}))
    assert none["found"] is False


def test_engine_layer_gate_script_exists():
    layer = REPO_ROOT / "pipeline" / "gates" / "run_engine_layer.sh"
    assert layer.is_file()
    adv = REPO_ROOT / "pipeline" / "gates" / "run_engine_adversarial.sh"
    assert adv.is_file()
    tpl = (
        REPO_ROOT
        / "pipeline"
        / "templates"
        / "engine_backend"
        / "test_engine_layer.cpp.tpl"
    )
    assert tpl.is_file()
    assert (REPO_ROOT / "pipeline" / "prompts" / "engine_backend" / "layers.md").is_file()
    assert (REPO_ROOT / "pipeline" / "prompts" / "engine_backend" / "layer_contracts.md").is_file()
    assert (REPO_ROOT / "pipeline" / "prompts" / "engine_backend" / "contract_tests.md").is_file()
    assert (REPO_ROOT / "pipeline" / "prompts" / "engine_backend" / "adversarial_tests.md").is_file()


def test_run_gate_sets_engine_context_and_out_env(monkeypatch):
    """Engine gate scripts require ACCEL_GATE_CONTEXT / ACCEL_GATE_OUT (not just the file on disk)."""
    from accel_pipeline.gates import run_gate

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("engine_postgres", REPO_ROOT)
    captured: dict = {}

    def fake_run(cmd, **kwargs):
        if not (isinstance(cmd, (list, tuple)) and cmd and str(cmd[0]).endswith("bash")):
            return real_run(cmd, **kwargs)
        captured["env"] = kwargs.get("env") or {}
        out = Path(captured["env"]["ACCEL_GATE_OUT"])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            '{"gate":"engine_store","status":"pass","notes":"ok"}',
            encoding="utf-8",
        )

        class Proc:
            returncode = 0
            stdout = ""
            stderr = ""

        return Proc()

    import subprocess as _sp

    real_run = _sp.run
    monkeypatch.setattr("accel_pipeline.gates.subprocess.run", fake_run)
    monkeypatch.setattr(
        "accel_pipeline.worker_isolation.ensure_worker_build_dir",
        lambda *a, **k: None,
    )
    result = run_gate(pc, "engine_store", 0, workload=workload, raise_on_failure=False)
    assert result.status == "pass"
    env = captured["env"]
    assert Path(env["ACCEL_GATE_CONTEXT"]).is_file()
    assert env["ACCEL_GATE_CONTEXT"].endswith("_gate_context.json")
    assert env["ACCEL_GATE_OUT"].endswith("engine_store.json")
    assert env.get("ACCEL_GATE_NAME") == "engine_store"
    assert "engines/postgres" in env["ACCEL_GATE_OUT"].replace("\\", "/")
    assert env.get("ACCEL_PG_DOCKER") == "1" or env.get("PGHOST") or env.get("ACCEL_PG_CONTAINER")
    ctx = json.loads(Path(env["ACCEL_GATE_CONTEXT"]).read_text(encoding="utf-8"))
    assert ctx.get("implementation_layers")
    assert ctx.get("host_runtime", {}).get("kind") == "docker"


def test_render_layer_critique_prompt():
    from accel_pipeline.stages.engine_backend import _layer_extra_context

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    assert w["layer_critique_max_rounds"] == 2
    assert w["layer_gate_max_rounds"] == 3
    layer = implementation_layers(w)[0]
    text = render_prompt(
        pc,
        "05_layer_critique.md",
        w,
        0,
        "critique_store",
        extra_context=_layer_extra_context(w, layer),
    )
    assert "engine_store" in text
    assert "store_roundtrip" in text
    assert "TryPublishPartition" in text
    assert "layer_contracts.md" in text
    assert "layer_critique_store.json" in text
    assert "{{layer_id}}" not in text
    assert "{{codex_tooling}}" not in text
    assert "Do **not** rewrite adapter sources" in text or "Do not rewrite" in text
    assert "smallest" in text.lower() or "minimal" in text.lower()
    assert "last action" in text.lower() or "not done until" in text.lower()


def test_run_until_artifact_retries_then_succeeds(tmp_path):
    from accel_pipeline.stages.engine_layer_critique import run_until_artifact

    path = tmp_path / "layer_critique_aux_backend.json"
    calls: list[str] = []

    def invoke(extra: str):
        calls.append(extra)
        if len(calls) == 2:
            path.write_text('{"status":"pass"}\n', encoding="utf-8")
        return object()

    run_until_artifact(artifact=path, invoke=invoke, attempts=2)
    assert len(calls) == 2
    assert calls[0] == ""
    assert "STOP EXPLORING" in calls[1]
    assert path.is_file()


def test_run_until_artifact_dry_run_does_not_retry(tmp_path):
    from accel_pipeline.stages.engine_layer_critique import run_until_artifact

    calls = {"n": 0}

    def invoke(extra: str):
        calls["n"] += 1
        return object()

    run_until_artifact(
        artifact=tmp_path / "missing.json", invoke=invoke, attempts=2, dry_run=True
    )
    assert calls["n"] == 1


def test_layer_critique_feedback_and_load(tmp_path):
    from accel_pipeline.gates import GateFailure
    from accel_pipeline.stages.engine_layer_critique import (
        format_layer_critique_feedback,
        load_layer_critique,
    )
    import pytest

    with pytest.raises(GateFailure, match="did not write"):
        load_layer_critique(tmp_path, "store")
    (tmp_path / "layer_critique_store.json").write_text(
        json.dumps(
            {
                "engine_name": "postgres",
                "layer": "store",
                "status": "FAIL",
                "summary": "Invalidate uses caller partition ids",
                "violations": [
                    {
                        "rule": "harness_gap",
                        "evidence": "TryPublish remaps partitions; harness only tests InvalidateAll",
                        "paths": ["backends/postgres/state/postgres_state_store.cpp"],
                    }
                ],
                "feedback_for_implementation": "Assert Invalidate(PartitionSet) after remap.",
            }
        ),
        encoding="utf-8",
    )
    critique = load_layer_critique(tmp_path, "store")
    assert critique["status"] == "fail"
    text = format_layer_critique_feedback(critique, round_idx=1)
    assert "Layer critique FAILED" in text
    assert "harness_gap" in text
    assert "Invalidate(PartitionSet)" in text


def test_critique_tools_are_read_only_plus_artifact_write():
    from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES, stage_tool_names, tool_usage_instructions

    names = stage_tool_names("critique_store")
    assert names == STAGE_TOOL_NAMES["layer_critique"]
    assert "write_file" in names
    assert "apply_patch" not in names
    assert "run_command" not in names
    usage = tool_usage_instructions("critique_store")
    assert "Layer critique workflow" in usage
    assert "layer_contracts.md" in usage
    assert "cat >" in usage or "LAST action" in usage or "last action" in usage.lower()


def test_layer_gate_feedback_includes_log_tail(tmp_path):
    from accel_pipeline.gates import GateResult
    from accel_pipeline.stages.engine_backend import format_layer_gate_feedback

    failed = GateResult(
        name="engine_txn",
        status="fail",
        data={
            "notes": "incompatible library: missing magic block",
            "log_tail": (
                "LOAD 'accelerator_codegen_pg'\n"
                "HINT: Extension libraries are required to use the PG_MODULE_MAGIC macro."
            ),
        },
        path=tmp_path / "engine_txn.json",
    )
    text = format_layer_gate_feedback(
        failed, layer_id="txn", round_idx=1, max_rounds=3
    )
    assert "Layer gate FAILED" in text
    assert "engine_txn" in text
    assert "PG_MODULE_MAGIC" in text
    assert "attempt: 1/3" in text
    assert "engine_txn.json" in text
    assert "### returncode" in text


def test_layer_gate_feedback_empty_log_tail_still_has_signal(tmp_path):
    from accel_pipeline.gates import GateResult
    from accel_pipeline.stages.engine_backend import format_layer_gate_feedback

    failed = GateResult(
        name="engine_txn",
        status="fail",
        data={
            "notes": "layer harness failed via docker exec: test_mysql_txn",
            "log_tail": "",
            "returncode": 134,
        },
        path=tmp_path / "engine_txn.json",
    )
    text = format_layer_gate_feedback(
        failed, layer_id="txn", round_idx=1, max_rounds=3
    )
    assert "SIGABRT" in text
    assert "test_mysql_txn" in text
    assert "(empty)" not in text


def test_layer_gate_fail_reinvokes_impl(tmp_path, monkeypatch):
    from accel_pipeline.gates import GateResult
    from accel_pipeline.stages import engine_backend as eb
    from accel_pipeline.stages.engine_backend import run_implementation_layer

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    layer = implementation_layers(w)[0]
    calls = {"impl": 0, "critique": 0, "gates": 0}
    user_messages: list[str] = []

    def fake_impl(**kwargs):
        calls["impl"] += 1
        user_messages.append(str(kwargs.get("user_message") or ""))
        return object()

    def fake_collect(*args, **kwargs):
        calls["gates"] += 1
        if calls["gates"] == 1:
            failed = GateResult(
                name="engine_store",
                status="fail",
                data={
                    "notes": "incompatible library: missing magic block",
                    "log_tail": (
                        "LOAD 'accelerator_codegen_pg'\n"
                        "HINT: Extension libraries are required to use the "
                        "PG_MODULE_MAGIC macro."
                    ),
                },
                path=tmp_path / "engine_store.json",
            )
            return [failed], failed
        return [], None

    def fake_critique(*args, **kwargs):
        calls["critique"] += 1
        (tmp_path / "layer_critique_store.json").write_text(
            json.dumps(
                {
                    "engine_name": "mysql",
                    "layer": "store",
                    "status": "pass",
                    "summary": "ok",
                    "violations": [],
                    "feedback_for_implementation": "",
                }
            )
            + "\n",
            encoding="utf-8",
        )

    monkeypatch.setattr(eb, "run_agent_stage", fake_impl)
    monkeypatch.setattr(eb, "run_post_stage_gates_collect", fake_collect)
    monkeypatch.setattr(eb, "run_layer_critique_agent", fake_critique)
    monkeypatch.setattr(eb, "query_artifacts_dir", lambda *a, **k: tmp_path)
    monkeypatch.setattr(eb, "append_experiment_log", lambda *a, **k: tmp_path)
    run_implementation_layer(
        pc,
        mc,
        "engine_mysql",
        0,
        {},
        False,
        layer,
        extra_user_message="keep AttachServices",
    )
    assert calls["impl"] == 2
    assert calls["gates"] == 2
    assert calls["critique"] == 1
    assert "keep AttachServices" in user_messages[0]
    assert "keep AttachServices" in user_messages[1]
    assert "PG_MODULE_MAGIC" in user_messages[1]
    assert "engine_store" in user_messages[1]
    assert (tmp_path / "layer_gate_store_feedback_round1.md").is_file()


def test_layer_gate_exhausted_rounds_raises(tmp_path, monkeypatch):
    import pytest
    from accel_pipeline.gates import GateFailure, GateResult
    from accel_pipeline.stages import engine_backend as eb
    from accel_pipeline.stages.engine_backend import run_implementation_layer

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    layer = implementation_layers(load_workload("engine_mysql", REPO_ROOT))[0]
    calls = {"impl": 0, "critique": 0}

    def fake_impl(**kwargs):
        calls["impl"] += 1
        return object()

    def fake_collect(*args, **kwargs):
        failed = GateResult(
            name="engine_store",
            status="fail",
            data={"notes": "missing magic block", "log_tail": "PG_MODULE_MAGIC"},
            path=tmp_path / "engine_store.json",
        )
        return [failed], failed

    def fake_critique(*args, **kwargs):
        calls["critique"] += 1

    monkeypatch.setattr(eb, "run_agent_stage", fake_impl)
    monkeypatch.setattr(eb, "run_post_stage_gates_collect", fake_collect)
    monkeypatch.setattr(eb, "run_layer_critique_agent", fake_critique)
    monkeypatch.setattr(eb, "query_artifacts_dir", lambda *a, **k: tmp_path)
    monkeypatch.setattr(eb, "append_experiment_log", lambda *a, **k: tmp_path)
    with pytest.raises(GateFailure, match="still failing after 3"):
        run_implementation_layer(pc, mc, "engine_mysql", 0, {}, False, layer)
    assert calls["impl"] == 3
    assert calls["critique"] == 0


def test_layer_critique_fail_reinvokes_impl(tmp_path, monkeypatch):
    from accel_pipeline.stages import engine_backend as eb
    from accel_pipeline.stages.engine_backend import run_implementation_layer

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    layer = implementation_layers(w)[0]
    calls = {"impl": 0, "critique": 0}
    user_messages: list[str] = []

    def fake_impl(**kwargs):
        calls["impl"] += 1
        user_messages.append(str(kwargs.get("user_message") or ""))
        return object()

    def fake_critique(*args, **kwargs):
        calls["critique"] += 1
        status = "fail" if calls["critique"] == 1 else "pass"
        (tmp_path / "layer_critique_store.json").write_text(
            json.dumps(
                {
                    "engine_name": "mysql",
                    "layer": "store",
                    "status": status,
                    "summary": "gap" if status == "fail" else "ok",
                    "violations": (
                        [{"rule": "harness_gap", "evidence": "Invalidate remap untested"}]
                        if status == "fail"
                        else []
                    ),
                    "feedback_for_implementation": (
                        "Add Invalidate(PartitionSet) after TryPublish remap."
                        if status == "fail"
                        else ""
                    ),
                }
            )
            + "\n",
            encoding="utf-8",
        )

    monkeypatch.setattr(eb, "run_agent_stage", fake_impl)
    monkeypatch.setattr(eb, "run_layer_critique_agent", fake_critique)
    monkeypatch.setattr(eb, "query_artifacts_dir", lambda *a, **k: tmp_path)
    monkeypatch.setattr(eb, "append_experiment_log", lambda *a, **k: tmp_path)
    run_implementation_layer(
        pc,
        mc,
        "engine_mysql",
        0,
        {},
        False,
        layer,
        skip_gates=True,
        extra_user_message="architecture audit: keep AttachServices",
    )
    assert calls["impl"] == 2
    assert calls["critique"] == 2
    assert "architecture audit: keep AttachServices" in user_messages[0]
    assert "architecture audit: keep AttachServices" in user_messages[1]
    assert "Invalidate(PartitionSet)" in user_messages[1]


def test_layer_critique_exhausted_rounds_raises(tmp_path, monkeypatch):
    import pytest
    from accel_pipeline.gates import GateFailure
    from accel_pipeline.stages import engine_backend as eb
    from accel_pipeline.stages.engine_backend import run_implementation_layer

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    layer = implementation_layers(w)[0]

    def fake_impl(**kwargs):
        return object()

    def fake_critique(*args, **kwargs):
        (tmp_path / "layer_critique_store.json").write_text(
            json.dumps(
                {
                    "engine_name": "mysql",
                    "layer": "store",
                    "status": "fail",
                    "summary": "still broken",
                    "violations": [{"rule": "contract", "evidence": "CAS ignored"}],
                    "feedback_for_implementation": "Honor TryPublishPartition CAS.",
                }
            )
            + "\n",
            encoding="utf-8",
        )

    monkeypatch.setattr(eb, "run_agent_stage", fake_impl)
    monkeypatch.setattr(eb, "run_layer_critique_agent", fake_critique)
    monkeypatch.setattr(eb, "query_artifacts_dir", lambda *a, **k: tmp_path)
    monkeypatch.setattr(eb, "append_experiment_log", lambda *a, **k: tmp_path)
    with pytest.raises(GateFailure, match="still failing after 2"):
        run_implementation_layer(
            pc, mc, "engine_mysql", 0, {}, False, layer, skip_gates=True
        )


def test_dry_run_impl_store_skips_critique(monkeypatch):
    from accel_pipeline.stages import engine_backend as eb

    called = {"critique": 0, "gates": 0}

    def fake_critique(*args, **kwargs):
        called["critique"] += 1

    def fake_collect(*args, **kwargs):
        called["gates"] += 1
        return [], None

    monkeypatch.setattr(eb, "run_layer_critique_agent", fake_critique)
    monkeypatch.setattr(eb, "run_post_stage_gates_collect", fake_collect)
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(
        pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="impl_store", skip_gates=False
    )
    assert called["critique"] == 0
    assert called["gates"] == 0
