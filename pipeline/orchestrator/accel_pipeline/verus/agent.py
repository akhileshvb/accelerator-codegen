"""Agent steps of the Verus pipeline: codex exec in the workspace, MCP tools for the rest.

Prompts are ``prompts/verus/<name>.md``, rendered with this module's own
context (never the C++ pipeline's ``build_prompt_context``, which carries C++
accelerator context).  ``{{name}}`` substitutes a variable and
``{{include:path}}`` inlines ``prompts/verus/<path>``.

Every agent step must resolve to the ``codex_exec`` provider
(``stage_providers`` in the models config); anything else is an error.

Testing without an LLM: ``ACCEL_VERUS_REPLAY=<dir>`` replays
``<dir>/<stage>/`` instead: its ``files/`` tree is copied over the workspace
and ``final.txt`` is the agent's final message (``<stage>_<n>`` for the n-th
call of a stage, when present).
"""

from __future__ import annotations

import dataclasses
import os
import re
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.verus.context import VerusContext


@dataclass
class AgentResult:
    ok: bool
    final_text: str
    seconds: float
    error: str = ""
    session_id: str = ""  # the codex session (resume it to continue with its context)


def render(ctx: VerusContext, prompt: str, extra: dict[str, str] | None = None) -> str:
    base = Path(ctx.pipeline_root) / "prompts" / "verus"
    variables = {
        "query": str(ctx.query),
        "benchmark": ctx.benchmark,
        "accel_id": ctx.accel_id,
        "crate": ctx.crate,
        "ffi_prefix": ctx.ffi_prefix,
        "workspace": ctx.workspace,
        **(extra or {}),
    }

    def expand(text: str, depth: int = 0) -> str:
        def inc(m: re.Match[str]) -> str:
            path = base / m.group(1).strip()
            return expand(path.read_text(encoding="utf-8"), depth + 1) if depth < 4 and path.is_file() else m.group(0)

        text = re.sub(r"\{\{include:([^}]+)\}\}", inc, text)
        text = re.sub(r"\{\{(\w+)\}\}", lambda m: variables.get(m.group(1), m.group(0)), text)
        return re.sub(r"\{\{file:([^}]+)\}\}", lambda m: inline_files(ctx, m.group(1).strip()), text)

    return expand((base / prompt).read_text(encoding="utf-8"))


INLINE_LIMIT = 60_000  # characters per inlined file


def inline_files(ctx: VerusContext, pattern: str) -> str:
    """`{{file:<path or glob>}}`: the workspace file(s) pasted into the prompt, so a
    reviewer has its material in one message instead of reading it piece by piece."""
    paths = sorted(p for p in ctx.ws.glob(pattern) if p.is_file()) if any(c in pattern for c in "*?[") \
        else [ctx.ws / pattern]
    out = []
    for path in paths:
        rel = path.relative_to(ctx.ws) if path.is_relative_to(ctx.ws) else path
        if not path.is_file():
            out.append(f"`{rel}`: (not present)")
            continue
        body = path.read_text(encoding="utf-8", errors="replace")
        if len(body) > INLINE_LIMIT:
            body = body[:INLINE_LIMIT] + f"\n... (truncated at {INLINE_LIMIT} characters; read the rest from the file)"
        lang = {".rs": "rust", ".sql": "sql", ".json": "json", ".cpp": "cpp", ".hpp": "cpp", ".h": "c",
                ".md": "markdown"}.get(path.suffix, "")
        out.append(f"`{rel}`:\n\n```{lang}\n{body.rstrip()}\n```")
    return "\n\n".join(out) if out else f"`{pattern}`: (no files)"


_calls: dict[str, int] = {}


def run_agent(
    ctx: VerusContext,
    *,
    stage: str,
    prompt: str,
    task: str,
    pipeline_config: Any,
    model_config: Any,
    model_overrides: dict[str, str] | None = None,
    extra: dict[str, str] | None = None,
    dry_run: bool = False,
    timeout_sec: int | None = None,
    resume: str = "",
) -> AgentResult:
    """Run one agent step (stage name ``verus_*``) on codex exec in the workspace.

    `resume`: a session id from an earlier step: continue that session (the
    agent keeps what it read and did) with `task` as the next message; the
    prompt file is not re-sent (pass prompt="").
    """
    _calls[stage] = _calls.get(stage, 0) + 1
    n = _calls[stage]
    text = render(ctx, prompt, extra) if prompt else ""
    pdir = ctx.art / "prompts"
    pdir.mkdir(parents=True, exist_ok=True)
    (pdir / f"{stage}_{n}.md").write_text(f"{text}\n\n## Task\n\n{task}\n", encoding="utf-8")
    if dry_run:
        return AgentResult(True, "(dry run)", 0.0)
    replay = os.environ.get("ACCEL_VERUS_REPLAY")
    if replay:
        return _replay(ctx, Path(replay), stage, n)

    from accel_pipeline.backends.base import StageRunRequest
    from accel_pipeline.backends.codex_exec import CodexExecBackend
    from accel_pipeline.models import make_model_id, resolve_agent_model, resolve_stage_provider
    from accel_pipeline.pipeline_log import log_stage_complete, log_stage_start, usage_from_metadata

    provider = resolve_stage_provider(stage, model_config)
    if provider != "codex_exec":
        raise RuntimeError(
            f"Verus pipeline step {stage} resolves to provider {provider!r}: every Verus agent step runs on "
            f"codex_exec (set stage_providers.{stage}: codex_exec, e.g. --models-config "
            "pipeline/config/models.codex.yaml)")
    # codex works in the workspace; its CODEX_HOME (and MCP launcher) stay the pipeline's
    codex_home = getattr(pipeline_config, "codex_home", None) or Path(ctx.pipeline_root) / ".codex"
    ws_config = dataclasses.replace(pipeline_config, repo_root=ctx.ws, codex_home=Path(codex_home))
    mc = model_config
    if timeout_sec:
        mc = dataclasses.replace(model_config, codex_timeout_sec=int(timeout_sec))
    os.environ["ACCEL_VERUS_CONTEXT"] = str(ctx.save())
    os.environ["ACCEL_PIPELINE_MODE"] = "verus"
    model_id = make_model_id(stage, pipeline_config, mc, model_overrides)
    agent_model = resolve_agent_model(stage, pipeline_config, mc, model_overrides)
    log_stage_start(ctx.root, ctx.query, stage=stage, model_id=model_id)
    backend = CodexExecBackend(pipeline_config=ws_config, model_config=mc, query=ctx.query, stage=stage,
                               workload_name=ctx.workload_name, art_dir=ctx.art)
    if str(ctx.setting("sandbox", "bwrap")) == "bwrap":
        from accel_pipeline.verus import sandbox
        backend.command_prefix = sandbox.prefix(ctx, ctx.ws)
        backend.task_dir = Path(ctx.run_dir) / "tmp"
    if resume:
        backend.resume_session_id = resume
    backend.prompt_via_stdin = True
    t0, started = time.monotonic(), time.time()
    result = backend.run_stage(StageRunRequest(stage=stage, instructions=text, user_message=task,
                                               agent_name=f"accel_{stage}", model_id=model_id,
                                               agent_model=agent_model, max_turns=10_000))
    seconds = round(time.monotonic() - t0, 1)
    from accel_pipeline.verus import usage as usage_mod

    sid = str(result.metadata.get("session_id") or "")
    use = usage_mod.session_usage(Path(codex_home), sid, started) or usage_from_metadata(result.metadata) or {}
    cost = usage_mod.cost_usd(model_id, use, ctx.setting("pricing", {}) or {})
    log_stage_complete(ctx.root, ctx.query, stage=stage, model_id=model_id, backend=result.backend,
                       duration_ms=result.metadata.get("duration_ms"),
                       usage={**use, **({"cost_usd": cost} if cost is not None else {})} or None)
    usage_mod.record(ctx.art, {"stage": stage, "n": n, "model": model_id, "ok": result.status == "finished",
                               "seconds": seconds, "session_id": sid, **use, "cost_usd": cost,
                               "at": time.strftime("%Y-%m-%dT%H:%M:%S")})
    (pdir / f"{stage}_{n}.out.txt").write_text(result.final_text or result.error or "", encoding="utf-8")
    return AgentResult(result.status == "finished", result.final_text or "", seconds, result.error or "",
                       str(result.metadata.get("session_id") or ""))


def _replay(ctx: VerusContext, root: Path, stage: str, n: int) -> AgentResult:
    d = root / f"{stage}_{n}"
    if not d.is_dir():
        d = root / stage
    if not d.is_dir():
        return AgentResult(True, "", 0.0)
    files = d / "files"
    if files.is_dir():
        for p in files.rglob("*"):
            if p.is_file():
                dst = ctx.ws / p.relative_to(files)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dst)
    final = d / "final.txt"
    return AgentResult(True, final.read_text(encoding="utf-8") if final.is_file() else "", 0.0)


def json_block(text: str) -> dict[str, Any] | None:
    """The last ```json block (or bare {...} line) of an agent's final message."""
    import json

    blocks = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", text or "", re.S)
    for b in reversed(blocks):
        try:
            return json.loads(b)
        except json.JSONDecodeError:
            continue
    for line in reversed((text or "").splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return None
