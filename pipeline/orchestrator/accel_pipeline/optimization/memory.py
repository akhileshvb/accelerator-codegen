"""What earlier attempts teach the next one.

One record store holds it: `optimization_memory.jsonl` in the query's artifacts, and a per-engine copy for the
lessons that outlive a query. A record says what was tried, on what evidence, and what it measured; its kind is
`trial` (a profile-guided trial began), `attempt` (it was judged), `technique` (a technique the agent reports) or
`revision` (a kept redesign of the state). Attempts are written once, when they are measured (`record_attempt`);
a clear reject is also kept in the engine's store as a lesson, tagged with the signals of its evidence.

Retrieval is by evidence: `signals_of(metrics)` names what the measurements show (a write-mix cliff, a weak read,
a validity-scan fan-out ...), and lessons and the curated precedents of the transformation catalog are ranked by
how many signals they share with it. `advise` turns the measurements, the kernel's static review and what
retrieval returns into the digest the agent reads (`optimization_runtime.md`); saturation claims are judged
against the trial records.
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.config import (
    PipelineConfig,
    load_query_analysis,
    pipeline_root,
    query_artifacts_dir,
    read_json,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
)
from accel_pipeline.optimization.bench import (
    RECOMMENDED_PHASES,
    WORK_COUNT_KEYS,
    best_valid_metrics,
    as_float,
    build_aux_profile_summary,
    extract_store_diagnostics,
    latency_from_metrics,
    primary_score_from_metrics,
    read_axes,
    write_mix_from_metrics,
)

MEMORY_FILE = "optimization_memory.jsonl"
SHARED_LIMIT = 100
CLEAR_REGRESS_PCT = -8.0  # a milder regression is noise, not a lesson


# The store

def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _clip(text: Any, limit: int) -> str:
    cleaned = " ".join(str(text or "").split())
    return cleaned if len(cleaned) <= limit else cleaned[: limit - 3] + "..."


def _read_records(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return [r for r in rows if isinstance(r, dict)]


def remember(art: Path, kind: str, **fields: Any) -> dict[str, Any]:
    """Append a record to the query's store."""
    record = {"kind": kind, "ts": _now(), **fields}
    art.mkdir(parents=True, exist_ok=True)
    with (art / MEMORY_FILE).open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    return record


def recall(art: Path, kind: str | None = None, *, limit: int | None = None) -> list[dict[str, Any]]:
    rows = [r for r in _read_records(art / MEMORY_FILE) if kind is None or r.get("kind") == kind]
    return rows[-limit:] if limit else rows


def shared_store_path(repo_root: Path, engine: str) -> Path:
    """Per engine: one engine's lessons never reach another engine's analysis."""
    return pipeline_root(repo_root) / "artifacts" / "shared" / engine / MEMORY_FILE


_SEED_LESSONS = (
    ("Do not treat RebuildAll-on-every-point-write as finished when 10% write-mix ≪ 1×",
     "aux; read already strong; write-mix cliff",
     "Prefer ApplyDelta / touched partitions; dual-axis score weights write_mix", ("write_mix_cliff",)),
    ("Do not put huge rebuild/scratch maps on the accel hit path",
     "read path regressions after maintenance refactors",
     "Hit latency can jump to 100ms+ while write-mix looks improved", ("slow_hit_path",)),
    ("Do not hardcode iteration-database cardinalities as live capacities on the hot path",
     "sizing maps/arrays/partitions",
     "Size from bind-time row_estimate / measured domain", ("capacity_sizing",)),
    ("Do not spend an attempt on ≤5% read micro-opts while write-mix cliff remains",
     "aux dual-axis optimization; read already ≫1× and 10% ≪1×",
     "Maintenance hypothesis usually moves the accept score more", ("write_mix_cliff", "read_strong")),
)


def _lesson_key(lesson: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", lesson.lower()).strip()


def _shared_records(repo_root: Path, engine: str) -> list[dict[str, Any]]:
    """The engine's lessons; a new store starts with the seed lessons."""
    path = shared_store_path(repo_root, engine)
    if not path.is_file():
        path.parent.mkdir(parents=True, exist_ok=True)
        seeds = [{"kind": "attempt", "verdict": "seed", "lesson": lesson, "when": when, "reason": evidence,
                  "signals": list(signals), "source": "seed", "ts": _now()} for lesson, when, evidence, signals in _SEED_LESSONS]
        path.write_text("".join(json.dumps(s, ensure_ascii=False) + "\n" for s in seeds), encoding="utf-8")
    return _read_records(path)


def signals_of(metrics: dict[str, Any] | None, analysis: dict[str, Any] | None = None) -> set[str]:
    """What the evidence shows, as tags records and catalog entries are matched on."""
    metrics = metrics or {}
    diag = extract_store_diagnostics(metrics)
    found: set[str] = set()
    if float(diag.get("validity_slots_scanned_median") or 0) >= 1000:
        found.add("high_validity_scan")
    if float(diag.get("partitions_read_per_query_median") or 0) >= 10:
        found.add("high_partition_read_fanout")
    if float(diag.get("valid_partitions") or 0) >= 1000:
        found.add("many_valid_partitions")
    if float(diag.get("accel_hit_median_us") or 0) >= 10_000:
        found.add("slow_hit_path")
    write_mix, read = write_mix_from_metrics(metrics), float(metrics.get("read_only_query_speedup") or 0)
    if 0 < write_mix < 1.0:
        found.add("write_mix_cliff")
    if read >= 5.0:
        found.add("read_strong")
    if analysis:
        text = " ".join(str(x) for x in (analysis.get("likely_bottlenecks") or []))
        if re.search(r"string|varchar|brand|container|nation", text, re.I):
            found.add("string_bind_param")
        if re.search(r"scalar|single.?row|aggregate", text, re.I):
            found.add("scalar_output")
    return found


def record_attempt(
    art: Path, *, repo_root: Path, workload: dict[str, Any] | None, query: int, attempt: Any,
    hypothesis: dict[str, Any] | str | None, verdict: str, reason: str = "", improvement_pct: float | None = None,
    metrics: dict[str, Any] | None = None, accepted: bool = False, kept: bool = False, **extra: Any,
) -> dict[str, Any]:
    """An attempt (or an exploratory A/B trial) was judged: written once, here. A clear reject of a stated
    approach is also a lesson for the engine's later queries."""
    hypo = hypothesis if isinstance(hypothesis, dict) else {"bottleneck": str(hypothesis or "")}
    signals = sorted(signals_of(metrics, None))
    record = remember(
        art, "attempt", attempt=attempt, hypothesis=hypo.get("bottleneck", ""), hypothesis_json=hypo or None,
        verdict=verdict, accepted=accepted, kept=kept, reason=reason, improvement_pct=improvement_pct,
        metrics=metrics or None, signals=signals, **extra,
    )
    approach = str(hypo.get("bottleneck") or hypo.get("why") or reason or "").strip()
    clear = verdict == "reject_broken" or (
        verdict == "reject_regress" and (improvement_pct is None or improvement_pct <= CLEAR_REGRESS_PCT))
    if clear and approach:
        _share_lesson(repo_root, str((workload or {}).get("engine_name") or ""), record, query, approach, reason)
    return record


def _share_lesson(repo_root: Path, engine: str, record: dict[str, Any], query: int, approach: str, reason: str) -> None:
    lesson = _clip(f"Avoid repeating failed approach: {approach}", 160)
    rows = _shared_records(repo_root, engine)
    if any(_lesson_key(str(r.get("lesson") or "")) == _lesson_key(lesson) for r in rows):
        return
    when = f"verdict={record['verdict']}" + (f"; Δ={record['improvement_pct']:+.1f}%" if record["improvement_pct"] is not None else "")
    why = str((record.get("hypothesis_json") or {}).get("why") or reason)
    rows.append({**record, "query": query, "lesson": lesson, "when": when, "reason": _clip(why, 160), "metrics": None,
                 "hypothesis_json": None, "source": "orchestrator"})
    path = shared_store_path(repo_root, engine)
    path.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows[-SHARED_LIMIT:]), encoding="utf-8")


def evidence_signals(art: Path, metrics: dict[str, Any] | None, analysis: dict[str, Any] | None) -> set[str]:
    """The signals of the current measurements and of the attempts that failed."""
    found = signals_of(metrics, analysis)
    for attempt in recall(art, "attempt"):
        if not attempt.get("accepted") and not attempt.get("kept"):
            found |= signals_of(attempt.get("metrics"), analysis)
    return found


def lessons(repo_root: Path, engine: str, signals: set[str], *, limit: int = 8) -> list[dict[str, Any]]:
    """The engine's lessons that share the most signals with the evidence (then the most recent), oldest first."""
    rows = [r for r in _shared_records(repo_root, engine) if r.get("lesson")]
    ranked = sorted(enumerate(rows), key=lambda ir: (len(signals & set(ir[1].get("signals") or ())), ir[0]), reverse=True)
    return [row for _, row in sorted(ranked[:limit])]


def lessons_markdown(rows: list[dict[str, Any]]) -> list[str]:
    if not rows:
        return []
    lines = ["## Negative memory (advisory — not authority)",
             "Prefer current profile/scoreboard. Ignore traps that contradict fresh measurements."]
    for row in rows:
        when = f" _{_clip(row.get('when'), 120)}_" if row.get("when") else ""
        lines.append(f"-{' Q' + str(row['query']) if row.get('query') else ''} {_clip(row['lesson'], 160)}{when}")
    return lines


# Precedents: the curated transformations of earlier queries

CATALOG_REL = "pipeline/catalog/aux_transformations.yaml"
MAX_PRECEDENTS = 3
MAX_EXCERPT_CHARS = 10_000
MAX_EXCERPT_LINES = 120
_PRECEDENT_DISCLAIMER = (
    "Precedents are historical; legality and fit must be judged against this query's "
    "correctness_facts and constraints. Catalog matching is not proof the same design applies."
)
_PROBLEM_KEYWORDS = {
    "high_validity_scan": ("validity", "scan"),
    "high_partition_read_fanout": ("fanout", "multi-partition", "partition"),
    "many_valid_partitions": ("per-part", "many"),
    "string_bind_param": ("string", "nation", "brand", "param"),
    "scalar_output": ("scalar",),
}


def _entry_signals(entry: dict[str, Any]) -> set[str]:
    problem = str(entry.get("problem", "")).lower()
    found = {sig for sig, words in _PROBLEM_KEYWORDS.items() if any(w in problem for w in words)}
    if float((entry.get("measured_effect") or {}).get("validity_slots_scanned_before") or 0.0) >= 1000:
        found.add("high_validity_scan")
    return found


def _excerpt(repo_root: Path, rel_path: str) -> str:
    path = repo_root / rel_path
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    text = "\n".join(f"{idx + 1:4d}| {line}" for idx, line in enumerate(lines[:MAX_EXCERPT_LINES]))
    return text + (f"\n... [{len(lines) - MAX_EXCERPT_LINES} more lines truncated]" if len(lines) > MAX_EXCERPT_LINES else "")


def precedents(repo_root: Path, signals: set[str], *, limit: int = MAX_PRECEDENTS) -> list[dict[str, Any]]:
    """Catalog entries ranked by shared signals; with no match, the first entries (a menu, not a match)."""
    path = repo_root / CATALOG_REL
    data = yaml.safe_load(path.read_text(encoding="utf-8")) if path.is_file() else None
    catalog = [e for e in data if isinstance(e, dict)] if isinstance(data, list) else []
    def match(entry: dict[str, Any]) -> float:
        shared = signals & _entry_signals(entry)
        return len(shared) / len(signals) if shared else 0.0

    ranked = sorted(((e, match(e)) for e in catalog), key=lambda item: item[1], reverse=True)
    chosen = [(e, s) for e, s in ranked if s > 0][:limit] or ranked[:1]
    cards, used = [], 0
    for entry, score in chosen:
        excerpts = []
        for rel in entry.get("excerpt_paths") or []:
            if used >= MAX_EXCERPT_CHARS or not (text := _excerpt(repo_root, str(rel))):
                continue
            text = text[: MAX_EXCERPT_CHARS - used] + ("\n... [excerpt truncated]" if len(text) > MAX_EXCERPT_CHARS - used else "")
            excerpts.append(f"// {rel}\n{text}")
            used += len(text)
        cards.append({**{k: entry.get(k) for k in ("id", "source_query", "problem", "transformation", "measured_effect",
                                                   "legality_constraints")}, "retrieval_score": round(score, 3), "excerpts": excerpts})
    return cards


def precedents_markdown(cards: list[dict[str, Any]]) -> str:
    if not cards:
        return ""
    lines = ["## Retrieved prior transformations (precedents — adapt, combine, or reject)", "", f"_{_PRECEDENT_DISCLAIMER}_", ""]
    for card in cards:
        lines += [f"### Q{card.get('source_query')}: {card.get('id', 'precedent')}",
                  f"- Problem: {card.get('problem', '')}", f"- Transformation: {card.get('transformation', '')}",
                  f"- Measured effect: {card.get('measured_effect', {})}",
                  f"- Legality/constraints: {card.get('legality_constraints', [])}"]
        for excerpt in card.get("excerpts") or []:
            lines += ["", "```cpp", excerpt, "```"]
        lines.append("")
    return "\n".join(lines)


def merge_playbook_proposal(repo_root: Path, query: int, *, workload: dict | None = None) -> bool:
    """The techniques the agent reports at the end of an iteration (`playbook_proposal.json`) become records."""
    art = query_artifacts_dir(repo_root, query, workload=workload)
    proposal = read_json(art / "playbook_proposal.json")
    techniques = [t for t in (proposal.get("techniques") or []) if isinstance(t, dict)]
    if not techniques and not str(proposal.get("summary") or "").strip():
        return False
    for row in techniques:
        if technique_id := str(row.get("technique_id") or row.get("id") or "").strip():
            remember(art, "technique", technique_id=technique_id, iteration=int(row.get("iteration") or proposal.get("iteration") or 0),
                     hypothesis=_clip(row.get("hypothesis"), 240), kept=row.get("kept"), speedup_delta=row.get("speedup_delta"),
                     profile_deltas=row.get("profile_deltas") or {})
    return True


# Saturation: the agent's claim that nothing is left, and the evidence it must come with.
#
# A stateful accelerator whose reads are barely faster than the engine's own SQL is not saturated, whatever the
# write axis says. Below the read-speedup floor a claim is honored only after the last N trials were each
# profile-guided and used distinct strategies, among them one structural and one low-level (vectorization /
# layout) change. If those were all tried and the floor is still missed, the first such claim triggers one
# structural re-analysis instead.

OPTIMIZATION_SATURATED = "OPTIMIZATION_SATURATED"
REANALYSIS_MARKER_FILE = "saturation_reanalysis_requested"
PROFILE_RUN_FILE = "last_profile_run"
BEST_VALID_FILE = "best_valid_baseline.json"
STRATEGY_STRUCTURAL, STRATEGY_LOW_LEVEL = "structural", "low_level"
STRATEGIES = (STRATEGY_STRUCTURAL, STRATEGY_LOW_LEVEL, "other")
_STRATEGY_HELP = {
    STRATEGY_STRUCTURAL: "a structural change (different state organization, partitioning or algorithm, so the read visits less state)",
    STRATEGY_LOW_LEVEL: "a low-level vectorization/layout change (SIMD with a scalar fallback, branch-free predicates, compact columnar layout, prefetch, no allocation or indirection in the hot loop)",
}
DEFAULT_MIN_READ_SPEEDUP = 20.0
DEFAULT_REQUIRED_ATTEMPTS = 8  # profile-guided, distinct trials before any saturation claim is honored
REDESIGN_HINT = (
    "If the profile shows the state itself is the problem (the read visits too much state, or the layout "
    "defeats SIMD), redesign it in this stage: a strategy=structural trial with a design_revision; it must "
    "pass the full suite and beat the best to stay."
)
HONOR, REJECT, REANALYZE = "honor", "reject", "reanalyze"


def parse_optimization_saturated(text: str) -> dict[str, Any] | None:
    """The payload if the agent declared OPTIMIZATION_SATURATED, else None."""
    if OPTIMIZATION_SATURATED not in (text or ""):
        return None
    payload: dict[str, Any] = {}
    if fence := re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL):
        try:
            raw = json.loads(fence.group(1))
            payload.update(raw if isinstance(raw, dict) else {})
        except json.JSONDecodeError:
            pass
    if not payload.get("reason"):
        payload["reason"] = next((line.strip()[:240] for line in text.splitlines() if "reason" in line.lower() and len(line.strip()) > 8),
                                 "agent declared optimization saturated")
    return {**payload, "status": "optimization_saturated"}


def early_stop_config(pipeline_config: Any) -> dict[str, Any]:
    raw = getattr(pipeline_config, "optimization_early_stop", None)
    cfg = dict(raw) if isinstance(raw, dict) else {}
    return {
        "enabled": bool(cfg.get("enabled", True)),
        "saturation_min_read_speedup": float(cfg.get("saturation_min_read_speedup", DEFAULT_MIN_READ_SPEEDUP)),
        "saturation_required_attempts": int(cfg.get("saturation_required_attempts", DEFAULT_REQUIRED_ATTEMPTS)),
    }


def mark_profile_run(art: Path) -> None:
    (art / PROFILE_RUN_FILE).write_text(_now(), encoding="utf-8")


def profile_guidance_error(art: Path, *, hot_spot: str, expected_gain: str, strategy: str) -> str:
    """Why a trial may not begin, or "" when it starts from a fresh profile and names its target."""
    marker, best = art / PROFILE_RUN_FILE, art / BEST_VALID_FILE
    if not marker.is_file() or (best.is_file() and best.stat().st_mtime > marker.stat().st_mtime + 0.01):
        return "run_profile on the current tree first (after an accept or restore the profile is stale)."
    if missing := [name for name, value in (("hot_spot", hot_spot), ("expected_gain", expected_gain)) if not value.strip()]:
        return f"name {' and '.join(missing)}: the hot function or loop from the run_profile output you target, and the gain you expect there."
    if strategy not in STRATEGIES:
        return f"strategy must be one of {', '.join(STRATEGIES)}."
    return ""


def record_trial(art: Path, *, trial_id: str, hot_spot: str, expected_gain: str, strategy: str) -> None:
    """A trial began from a profile: what it targets and how."""
    remember(art, "trial", trial_id=trial_id, hot_spot=hot_spot.strip(), expected_gain=expected_gain.strip(), strategy=strategy)


def missing_saturation_work(trials: list[dict[str, Any]], required: int) -> list[str]:
    """What the last `required` profile-guided trials still lack for a saturation claim."""
    recent = trials[-required:] if required > 0 else []
    missing: list[str] = []
    if len(recent) < required:
        missing.append(f"{required - len(recent)} more profile-guided trial(s): only {len(recent)} of the {required} required "
                       "began from run_profile output (begin_ab_trial with hot_spot, expected_gain and strategy)")
    if recent and len({(t.get("strategy"), str(t.get("hot_spot", "")).strip().lower()) for t in recent}) < len(recent):
        missing.append("distinct strategies: two of the last trials targeted the same hot spot the same way")
    present = {t.get("strategy") for t in recent}
    missing += [f"one trial with strategy={s}: {_STRATEGY_HELP[s]}" for s in (STRATEGY_STRUCTURAL, STRATEGY_LOW_LEVEL) if s not in present]
    return missing


def judge_saturation_claim(pipeline_config: Any, art: Path, payload: dict[str, Any] | None) -> tuple[str, str, dict[str, Any]]:
    """Decide an OPTIMIZATION_SATURATED claim: (HONOR | REJECT naming what is missing | REANALYZE, detail, config)."""
    cfg = early_stop_config(pipeline_config)
    if not cfg["enabled"]:
        return REJECT, "early_stop disabled in config", cfg
    reason = str(payload["reason"])[:300] if payload and payload.get("reason") else "agent judged optimization saturated"
    floor = cfg["saturation_min_read_speedup"]
    speedup = as_float((read_json(art / BEST_VALID_FILE).get("metrics") or {}).get("read_only_query_speedup")) or 0.0
    below_floor = 0 < speedup < floor
    if missing := missing_saturation_work(recall(art, "trial"), cfg["saturation_required_attempts"]):
        where = (f"read speedup over the engine's own SQL is {speedup:.2f}x, below the {floor:g}x floor; " if below_floor
                 else "not saturated yet (reads, writes and the state build all still count); ")
        return REJECT, where + "still to do: " + "; ".join(missing) + ". " + REDESIGN_HINT, cfg
    if not below_floor:
        return HONOR, reason, cfg
    if (art / REANALYSIS_MARKER_FILE).exists():
        return HONOR, f"{reason} (read speedup {speedup:.2f}x; structural re-analysis already spent)", cfg
    return (REANALYZE, f"read speedup {speedup:.2f}x is below the {floor:g}x floor after profile-guided structural and "
            f"low-level attempts; {reason}", cfg)


# Redesigns of the aux state inside the optimization stage. A kept redesign is written to the design record the
# audit reads: what the profile showed, the old and new organization, the measured effect.

REVISION_FIELDS = ("profile_evidence", "old_organization", "new_organization", "read_work_ratio")


def design_revision_error(hypothesis: dict[str, Any] | None) -> str:
    revision = (hypothesis or {}).get("design_revision")
    if not isinstance(revision, dict):
        return "a structural attempt must carry design_revision in its hypothesis JSON: " + ", ".join(REVISION_FIELDS)
    missing = [key for key in REVISION_FIELDS if not str(revision.get(key) or "").strip()]
    return f"design_revision is missing: {', '.join(missing)}" if missing else ""


def record_design_revision(art: Path, *, attempt_id: str, hypothesis: dict[str, Any] | None,
                           before: dict[str, Any], after: dict[str, Any]) -> None:
    revision = (hypothesis or {}).get("design_revision") or {}
    remember(art, "revision", attempt_id=attempt_id, **{key: revision.get(key) for key in REVISION_FIELDS}, measured={
        "read_speedup_before": before.get("read_only_query_speedup"), "read_speedup_after": after.get("read_only_query_speedup"),
        "write_mix_before": before.get("write_mix_workload_speedup"), "write_mix_after": after.get("write_mix_workload_speedup"),
    })


def design_revisions_markdown(art: Path) -> str:
    """Revisions made during optimization, for the audit: they supersede the analysis's design."""
    revisions = recall(art, "revision")
    if not revisions:
        return ""
    lines = ["## Design revisions made during optimization (the final design; they supersede the analysis)", ""]
    for entry in revisions:
        m = entry.get("measured") or {}
        lines += [
            f"- `{entry.get('attempt_id')}`: {entry.get('old_organization')} -> {entry.get('new_organization')}",
            f"  - profile evidence: {entry.get('profile_evidence')}",
            f"  - read_work_ratio: {entry.get('read_work_ratio')}; read speedup {m.get('read_speedup_before')} -> "
            f"{m.get('read_speedup_after')}, write mix {m.get('write_mix_before')} -> {m.get('write_mix_after')}",
        ]
    return "\n".join(lines) + "\n"


# The scoreboard: the one board A/B decisions compare against

SCOREBOARD_FILE = "optimization_scoreboard.json"


def profile_freshness(art: Path, *, repo_root: Path | None = None, editable_rel_paths: list[str] | None = None) -> dict[str, Any]:
    """Whether the profile artifacts are older than the editable sources or the best valid baseline."""
    mtimes = {name: (art / name).stat().st_mtime if (art / name).is_file() else 0.0
              for name in ("profile_summary.json", "custom_profile.json", BEST_VALID_FILE)}
    profile, custom, best = mtimes["profile_summary.json"], mtimes["custom_profile.json"], mtimes[BEST_VALID_FILE]
    source = max((p.stat().st_mtime for rel in (editable_rel_paths or []) if repo_root and (p := repo_root / rel).is_file()), default=0.0)
    reasons: list[str] = []
    if not profile:
        reasons.append("missing profile_summary.json — call run_profile")
    if source > 0 and profile > 0 and source > profile + 0.01:
        reasons.append("editable sources newer than profile_summary.json — re-run run_profile on the CURRENT tree before hypothesizing")
    if best > 0 and profile > 0 and best > profile + 0.01:
        reasons.append("best_valid_baseline newer than profile_summary.json — profile is from an older candidate; "
                       "re-run run_profile after restore/accept")
    stale = bool(reasons)
    if profile and not custom:
        reasons.append("no custom_profile.json — add split timers/counts (phase+us+rows/groups) "
                       "gated on ACCEL_AUX_CUSTOM_PROFILE / ACCEL_BENCH_PROFILE")
    elif custom > 0 and source > custom + 0.01:
        stale = True
        reasons.append("custom_profile.json older than sources — remeasure")
    return {"checked_at": _now(), "stale": stale, "reasons": reasons, "profile_summary_mtime": profile or None,
            "custom_profile_mtime": custom or None, "best_valid_mtime": best or None, "editable_sources_mtime": source or None}


def build_scoreboard(art: Path, *, is_aux: bool, last_metrics: dict[str, Any] | None = None) -> dict[str, Any]:
    """The compare-against board for A/B decisions and digests."""
    best_valid = read_json(art / BEST_VALID_FILE)
    fair = read_json(art / "aux_bench_summary.json") if is_aux else {}
    profile, baseline, state = read_json(art / "profile_summary.json"), read_json(art / "baseline_summary.json"), read_json(art / "optimization_state.json")
    best_metrics = dict(best_valid.get("metrics") or {})
    if best_valid.get("score") is not None:
        best_metrics.setdefault("optimization_score", best_valid["score"])
    measured = dict(last_metrics or profile or baseline or {})
    # Recomputed from the metrics, so a change of the dual-axis weights applies to a stored best valid baseline.
    best_score = primary_score_from_metrics(best_metrics, is_aux=is_aux)
    if best_score <= 0:
        best_score = as_float(best_valid.get("score")) or 0.0
    if best_score <= 0:
        best_score = primary_score_from_metrics(
            dict(state.get("best_summary") or {}) if is_aux else {"speedup_median": state.get("best_speedup_median")}, is_aux=is_aux)
    # Baseline/profile often omit the write mix while best_valid has it: inherit it for scoring only, so the
    # improvement compares like units.
    if is_aux and measured and best_metrics and write_mix_from_metrics(measured) <= 0 < write_mix_from_metrics(best_metrics):
        measured["write_mix_workload_speedup"] = best_metrics.get("write_mix_workload_speedup")
        if best_metrics.get("write_mix_pct") is not None:
            measured.setdefault("write_mix_pct", best_metrics["write_mix_pct"])
    measured_score = primary_score_from_metrics(measured, is_aux=is_aux)
    metric = "dual_axis_weighted(read^0.75 * write_mix^0.25)" if is_aux else "speedup_median"
    return {
        "score_metric": metric,
        "compare_against": "best_valid_score",
        "best_valid_score": best_score or None,
        "best_valid_attempt_id": best_valid.get("attempt_id"),
        "best_valid_hit_us": latency_from_metrics(best_metrics, is_aux=is_aux),
        "last_measured_score": measured_score or None,
        "last_measured_hit_us": latency_from_metrics(measured, is_aux=is_aux),
        "improvement_pct_vs_best_valid": round(100.0 * (measured_score - best_score) / best_score, 4) if best_score > 0 and measured_score > 0 else None,
        "read_speedup": {"best_valid": read_axes(best_metrics), "last_measured": read_axes(measured)} if is_aux else None,
        "fair_sweep_score": (primary_score_from_metrics(fair, is_aux=True) if fair else 0.0) or None,
        "guidance": (f"A/B keep/regress and yield decisions must use best_valid_score ({metric}). Ignore ephemeral local "
                     "baselines that disagree with best_valid. After accept/restore, call run_profile before new hyps."),
    }


def write_scoreboard(art: Path, board: dict[str, Any]) -> Path:
    art.mkdir(parents=True, exist_ok=True)
    (art / SCOREBOARD_FILE).write_text(json.dumps({**board, "updated_at": _now()}, indent=2), encoding="utf-8")
    return art / SCOREBOARD_FILE


def custom_profiling_contract(*, is_aux: bool) -> dict[str, Any]:
    env = "ACCEL_AUX_CUSTOM_PROFILE" if is_aux else "ACCEL_BENCH_PROFILE"
    return {
        "env_gate": env,
        "line_prefix": "ACCEL_CUSTOM_PROFILE",
        "required_fields": ["phase", "us"],
        "recommended_phases": list(RECOMMENDED_PHASES),
        "recommended_count_keys": list(WORK_COUNT_KEYS),
        "example": 'ACCEL_CUSTOM_PROFILE {"phase":"sort","us":120.5,"emitted_rows":180,"groups":180}',
        "rules": [
            f'Gate all instrumentation on getenv("{env}") — no-op when unset.',
            "Emit one JSON object per timed region (and optional per-query totals).",
            "Always include work counts (rows/groups/items/bytes) with timers.",
            "Re-run run_profile after every accept/restore before a new hypothesis.",
            "Hypothesis must cite dominant_phase + counts from a non-stale digest.",
        ],
    }


# Static review of the kernel's hot path

_HOT_FUNC_RE = re.compile(
    r"\b(?:void|bool)\s+(?:ProcessNum\w*|Accumulate\w*|ConsumeNum\w*|Aggregate\w*|Insert\w*Row|Process\w*Chunk\w*|RunNum\w*Pass)\s*\(")
_KEY = r"[^>]*(?:std::string|NameCikKey|GroupKey)[^>]*"
_STRING_KEYED_AGG_MAP_RE = re.compile(rf"unordered_map\s*<\s*{_KEY},\s*(?:double|float|int(?:32|64)_t|size_t|duckdb::idx_t)")
_STRING_KEYED_INTERN_MAP_RE = re.compile(rf"unordered_map\s*<\s*{_KEY},\s*uint32_t")
_STRING_HASH_RE = re.compile(r"std::hash\s*<\s*std::string\s*>")
_TO_STORED_STRING_RE = re.compile(r"\bToStoredString\s*\(")
_BUILD_PHASE_HINTS = re.compile(r"\b(InsertSubRow|InternGroup|Build\w*|RunSubScan|ConsumeSubChunk)\b")


def _hot_path_text(text: str) -> str | None:
    """The bodies of the functions that usually sit on the per-row consume/aggregate path (None: there are none)."""
    bodies = []
    for match in _HOT_FUNC_RE.finditer(text):
        brace = text.find("{", match.end())
        if brace < 0:
            continue
        depth, end = 0, brace
        for idx in range(brace, len(text)):
            depth += (text[idx] == "{") - (text[idx] == "}")
            if depth == 0:
                end = idx + 1
                break
        bodies.append(text[match.start(): end])
    return "\n".join(bodies) if bodies else None


def review_kernel(kernel_path: Path) -> dict[str, Any]:
    """Hot-path smells of a kernel translation unit, the first of each kind."""
    if not kernel_path.is_file():
        return {"kernel_path": str(kernel_path), "smells": [], "status": "missing"}
    text = kernel_path.read_text(encoding="utf-8", errors="replace")
    hot = _hot_path_text(text)
    smells: dict[str, dict[str, Any]] = {}

    def add(smell_id: str, severity: str, message: str, suggestion: str, line: int | None = None) -> None:
        smells.setdefault(smell_id, {"id": smell_id, "severity": severity, "line": line, "message": message, "suggestion": suggestion})

    agg_map = _STRING_KEYED_AGG_MAP_RE.search(text)
    if agg_map:
        add("string_keyed_hash_map", "high",
            "Kernel declares a hash map with string-composite keys mapped to numeric aggregates — typical per-row GROUP BY update pattern.",
            "Intern GROUP BY / dimension keys during build or bind (sub scan, dimension table load). Store `uint32_t group_id` "
            "on lookup rows; update `std::vector<double> group_sums` on the consume path.", text.count("\n", 0, agg_map.start()) + 1)
    if _STRING_KEYED_INTERN_MAP_RE.search(text) and hot and re.search(r"name_cik_to_group\.(?:find|emplace|try_emplace|\[)", hot):
        add("intern_map_on_hot_path", "medium",
            "Build-time intern map may be probed from a consume function — resolve group_id during sub/build only.",
            "Store `group_id` on lookup rows (e.g. FilingInfo) during sub scan; never touch the intern map inside ProcessNum*.")
    if _STRING_HASH_RE.search(text) and (agg_map or _STRING_HASH_RE.search(hot or "")):
        add("std_string_hash", "medium", "Uses `std::hash<std::string>` — expensive on a hot aggregation path.",
            "Replace string hashing with build-time interning to dense integer group ids.")
    if hot is not None:
        if _TO_STORED_STRING_RE.search(hot):
            add("heap_string_in_hot_path", "high",
                "`ToStoredString` / heap string allocation appears inside a likely consume/aggregate function.",
                "Move string materialization to build phase only; pass `string_t` or interned ids through the num scan loop.")
        if re.search(r"\.find\s*\(", hot) and agg_map:
            add("map_find_in_hot_path", "high",
                "Hash map `.find()` in a consume/aggregate function while string-keyed maps are present — likely per-row probe cost.",
                "Replace per-row map lookup with O(1) indexing via interned `group_id` or integer slot id resolved once at build time.")
    elif _TO_STORED_STRING_RE.search(text) and not _BUILD_PHASE_HINTS.search(text):
        add("heap_string_allocation", "medium", "Kernel allocates stored strings; verify this is build-phase only.",
            "Confine `ToStoredString` to sub/build paths, not num aggregation loops.")
    return {"kernel_path": str(kernel_path), "status": "ok", "smells": list(smells.values())}


def kernel_review_lines(review: dict[str, Any]) -> list[str]:
    smells = review.get("smells") or []
    if not smells:
        return ["- No high-confidence hot-path structure smells detected in kernel source."]
    lines = ["- **Static kernel review** (hints only — confirm with profile + a targeted read before acting):"]
    for smell in smells[:6]:
        loc = f" (line {smell['line']})" if smell.get("line") else ""
        lines.append(f"  - [{smell.get('severity', 'info')}]{loc} {smell.get('message', '')}")
        if smell.get("suggestion"):
            lines.append(f"    → {smell['suggestion']}")
    return lines


# Advice: the measurements, read as a digest for the agent

_SIGNAL_TIER = {
    "generic_fallback": "reduce_decode_materialize", "generic_rows": "reduce_decode_materialize",
    "generic_chunks": "reduce_decode_materialize", "vector_fast_paths": "specialize_vector_layout",
    "scan_amplification": "exploit_selectivity", "semi_join": "exploit_selectivity", "early_filter": "exploit_selectivity",
    "group_interning": "hash_aggregation_state", "string_keyed": "hash_aggregation_state", "topk_heap": "hash_aggregation_state",
    "fused": "scan_operator_fusion", "build_scan_order": "eliminate_unnecessary_work",
}
TIERS = ("eliminate_unnecessary_work", "exploit_selectivity", "reduce_decode_materialize", "scan_operator_fusion",
         "specialize_vector_layout", "hash_aggregation_state", "micro_optimization")

# The technique menu; the ones the profile points at are starred and listed first.
TECHNIQUES = {
    "early_filter_pushdown": "Move sub/pre/num predicates into NativeScanSpec or runtime TableFilterSet",
    "native_scan_column_projection": "Project only join/filter/aggregate columns in NativeScanSpec — fewer columns per chunk",
    "build_scan_order": "Build filtered dimension tables first (sub→pre) before the wide fact scan (num)",
    "semi_join_pushdown": "After building a filtered dimension, push join-key membership into the fact scan (BFTableFilter / bloom on scan columns)",
    "multi_column_scan_filters": "Stack multiple BFTableFilter entries on one fact scan (e.g. join key + secondary dimension column) to skip chunks before vector decode",
    "qualifying_composite_key_set": "Build a hash set of composite keys seen in an earlier phase; O(1) reject on the fact consume path before expensive probes",
    "deferred_dimension_scan": "Defer tag/pre resolution: scan tag or pre only for keys seen in an earlier build phase",
    "nested_lookup_by_adsh": "Partition pre/tag lookups by adsh (flat or nested map) to shrink probe working set per num row",
    "vector_fast_paths": "DICTIONARY/FLAT chunk dispatch via SelectProjectedColumn; avoid UnifiedVectorFormat per row",
    "specialized_chunk_handlers": "Separate handlers for all-flat, all-dict, and mixed layouts instead of one generic loop",
    "group_interning": "Intern GROUP BY keys at build time; consume path uses uint32_t group_id + dense sum/count vectors",
    "packed_group_keys": "Pack interned dimension ids into a single integer group key for O(1) indexing",
    "fixed_slots_with_overflow": "Dense fixed slots for hot groups + hash overflow for rare groups",
    "join_multiplicity": "Track (adsh,tag,version) multiplicity when SQL join duplicates rows — avoid under-counting",
    "build_phase_strings": "Confine ToStoredString / heap string allocation to sub/pre build — not num consume",
    "integer_fingerprint_keys": "Replace std::string join keys with fixed-width fingerprints (AdshKey, TagVersionKey, etc.)",
    "avoid_std_string_hash": "No std::hash<std::string> on hot paths — use intern tables or fingerprints resolved at build",
    "fused_consume_single_pass": "Single fused num pass: filter → dimension probe → aggregate without revisiting rows",
    "split_probe_vs_aggregate": "Separate timers/logic for rejected probes vs qualifying aggregates to find true hot work",
    "flat_pre_entry_arrays": "Store matching pre rows as contiguous arrays per adsh/tv_id — reduce pointer chasing in inner loop",
    "topk_heap_finalize": "Maintain a size-LIMIT heap during aggregation instead of sorting all groups at finalize",
    "partial_sort_output": "partial_sort or nth_element for ORDER BY … LIMIT instead of full sort of active_groups",
    "lazy_output_strings": "Defer resolving interned ids to VARCHAR until ProduceOutput — not during aggregation",
    "reserve_from_row_estimate": "Size hash maps and intern tables from bind-time row_estimate / GetNativeScanTableRowCount",
    "column_so_layout": "Keep hot per-row fields contiguous (struct-of-arrays or packed structs) for cache locality",
    "bind_time_filter_constants": "Hoist literal filters (stmt='IS', uom='USD', fy=2023) to bind-time constants and memcmp fast paths",
    "fused_production_path": "Wire fused kernel through the package's create_fused_kernel / fused_scan hooks so benchmark and gates use the same path",
    "coordinated_bundle": "Ship 2–4 related techniques per A/B trial — not isolated one-liners",
}
_DEFAULT_MENU_ORDER = (
    "coordinated_bundle", "build_scan_order", "group_interning", "vector_fast_paths", "early_filter_pushdown",
    "fused_consume_single_pass", "semi_join_pushdown", "multi_column_scan_filters", "qualifying_composite_key_set",
    "nested_lookup_by_adsh", "specialized_chunk_handlers", "flat_pre_entry_arrays", "topk_heap_finalize",
    "reserve_from_row_estimate", "native_scan_column_projection", "deferred_dimension_scan", "packed_group_keys",
    "lazy_output_strings", "bind_time_filter_constants", "fused_production_path",
)
_SMELL_TECHNIQUES = {
    "string_keyed_hash_map": ("group_interning", "packed_group_keys", "avoid_std_string_hash"),
    "map_find_in_hot_path": ("group_interning", "packed_group_keys", "avoid_std_string_hash"),
    "heap_string_in_hot_path": ("build_phase_strings",), "heap_string_allocation": ("build_phase_strings",),
    "std_string_hash": ("avoid_std_string_hash",),
}


def _tier_for_signal(signal: str) -> str:
    lower = signal.lower()
    return next((tier for key, tier in _SIGNAL_TIER.items() if key in lower), "micro_optimization")


def _facts(profile_phases: dict[str, float], counters: dict[str, Any]) -> dict[str, float]:
    """The numbers the base-kernel advice reasons about, zero when the profile lacks them."""
    f = {k: float(counters.get(k) or 0) for k in ("pre_rows", "generic_rows", "qualifying_rows", "distinct_inserts",
                                                   "flat_chunks", "dict_chunks", "generic_chunks")}
    f.update({k: float(profile_phases.get(k) or 0) for k in ("num_scan_us", "pre_consume_us", "sub_scan_us", "finalize_us",
                                                            "pre_scan_us", "pre_table_scan_us")})
    f["aggregate_us"] = float(profile_phases.get("num_aggregate_us") or 0) or f["pre_consume_us"]
    f["chunks"] = f["flat_chunks"] + f["dict_chunks"] + f["generic_chunks"]
    f["total_phase_us"] = sum(profile_phases.values()) or 1.0
    return f


def _technique_menu(f: dict[str, float], dominant: str | None, smells: list[dict[str, Any]], speedup: float) -> list[str]:
    boosted: list[str] = []
    ratio = lambda a, b: f[a] / f[b] if f[b] > 0 else 0.0  # noqa: E731
    rules = [
        (ratio("pre_rows", "qualifying_rows") >= 3 and f["pre_rows"] > 0,
         ("early_filter_pushdown", "build_scan_order", "semi_join_pushdown", "multi_column_scan_filters",
          "qualifying_composite_key_set", "deferred_dimension_scan", "nested_lookup_by_adsh")),
        (f["generic_rows"] > 0 and ratio("generic_rows", "pre_rows") >= 0.1, ("vector_fast_paths", "specialized_chunk_handlers")),
        (f["generic_chunks"] > 0 and ratio("generic_chunks", "chunks") >= 0.1, ("vector_fast_paths",)),
        (f["num_scan_us"] > 0 and f["aggregate_us"] / f["num_scan_us"] >= 0.35,
         ("fused_consume_single_pass", "split_probe_vs_aggregate", "flat_pre_entry_arrays")),
        (dominant in {"sub_scan_us", "pre_table_scan_us"} or f["sub_scan_us"] + f["pre_table_scan_us"] > 0.2 * f["total_phase_us"],
         ("build_phase_strings", "group_interning", "integer_fingerprint_keys", "reserve_from_row_estimate")),
        (dominant == "finalize_us" or (f["finalize_us"] > 0 and f["pre_scan_us"] > 0 and f["finalize_us"] / f["pre_scan_us"] >= 0.12),
         ("topk_heap_finalize", "partial_sort_output", "lazy_output_strings")),
        (f["qualifying_rows"] > 0 and f["distinct_inserts"] > 0 and f["distinct_inserts"] / f["qualifying_rows"] < 0.5,
         ("group_interning", "packed_group_keys", "fixed_slots_with_overflow")),
        (speedup < 0.5, ("coordinated_bundle",)),
    ]
    for hit, ids in rules:
        boosted += [i for i in ids if hit]
    for smell in smells:
        boosted += _SMELL_TECHNIQUES.get(str(smell.get("id", "")), ())
    starred = list(dict.fromkeys(boosted))
    rest = [i for i in (*_DEFAULT_MENU_ORDER, *TECHNIQUES) if i not in starred]
    return ([f"{i} ★: {TECHNIQUES[i]}" for i in starred] + [f"{i}: {TECHNIQUES[i]}" for i in dict.fromkeys(rest)])[:24]


def diagnose(
    baseline: dict[str, Any], profile: dict[str, Any], *, best_speedup: float = 0.0,
    analysis: dict[str, Any] | None = None, kernel_review: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """A base kernel's gate measurements read as findings, gaps, questions to investigate and a technique menu."""
    speedup = float(baseline.get("speedup_median") or 0.0)
    ref_ms, acc_ms = baseline.get("reference_median_ms"), baseline.get("accelerator_median_ms")
    if ref_ms is None and baseline.get("reference_median_us") is not None:
        ref_ms = float(baseline["reference_median_us"]) / 1000.0
    if acc_ms is None and baseline.get("accelerator_median_us") is not None:
        acc_ms = float(baseline["accelerator_median_us"]) / 1000.0
    phases = {k: float(v) for k, v in (profile.get("phases_us") or {}).items() if v is not None}
    for key in ("sub_scan_us", "num_scan_us", "num_aggregate_us"):
        if key not in phases and (profile.get("raw") or {}).get(key) is not None:
            phases[key] = float(profile["raw"][key])
    counters = profile.get("counters") or {}
    f = _facts(phases, counters)
    smells = list((kernel_review or {}).get("smells") or [])
    regressed = best_speedup > 0 and speedup + 0.01 < best_speedup

    findings: list[str] = []
    lacking: list[str] = []
    if regressed:
        lacking.append(f"REGRESSION: current {speedup:.3f}x vs best {best_speedup:.3f}x — the pipeline auto-restores the best kernel snapshot before the next iteration.")
    if speedup < 1.0:
        lacking.append(f"Accelerator is slower than DuckDB: speedup_median={speedup:.3f}x"
                       + (f" (ref≈{ref_ms:.1f}ms vs accel≈{acc_ms:.1f}ms)." if ref_ms and acc_ms else "."))
    elif regressed:
        lacking.append(f"Slower than best snapshot: speedup_median={speedup:.3f}x vs best {best_speedup:.3f}x.")
    else:
        findings.append(f"Beats DuckDB: speedup_median={speedup:.3f}x.")
        if best_speedup > 0 and speedup <= best_speedup + 0.01:
            lacking.append(f"At or below best snapshot ({best_speedup:.3f}x) — ship a new bundle to improve further.")
        else:
            findings.append("New best vs DuckDB — keep optimizing while improvements remain.")
    dominant, dominant_us = max(phases.items(), key=lambda kv: kv[1]) if phases else (None, 0.0)
    signals: list[str] = []
    questions: list[str] = []

    def ask(question: str) -> None:
        if question not in questions:
            questions.append(question)

    has_profile = bool(phases) or f["pre_rows"] > 0 or f["qualifying_rows"] > 0
    if not has_profile:
        signals.append("profile_missing: no phase timings or row counters from profile gate")
        ask("Profile gate returned no counters — verify benchmark supports ACCEL_BENCH_PROFILE on the fused production path.")
    if dominant:
        pct = 100.0 * dominant_us / f["total_phase_us"]
        findings.append(f"Dominant phase: {dominant} = {dominant_us:.0f} us ({pct:.0f}% of profiled scan path).")
        signals.append(f"dominant_phase={dominant} ({dominant_us:,.0f} us)")
    for name in ("pre_rows", "qualifying_rows", "distinct_inserts"):
        if f[name] > 0:
            signals.append(f"{name}={int(f[name]):,}")
    if f["pre_rows"] > 0 and f["generic_rows"] > 0:
        pct = 100.0 * f["generic_rows"] / f["pre_rows"]
        findings.append(f"generic_fallback_rows={int(f['generic_rows'])} ({pct:.1f}% of {int(f['pre_rows'])} scanned rows).")
        lacking.append(f"Almost all rows use the slow generic vector path ({pct:.0f}%); DICTIONARY/FLAT fast paths are not active.")
        signals.append(f"generic_fallback_rows={int(f['generic_rows']):,} ({pct:.1f}% of pre_rows)")
        ask("Where does the kernel fall back to UnifiedVectorFormat or a generic per-row loop? "
            "Search the kernel for `UnifiedVectorFormat` and chunk dispatch.")
    elif f["generic_rows"] > 1_000_000:
        findings.append(f"generic_fallback_rows={int(f['generic_rows'])} (pre_rows counter missing in profile CSV).")
        lacking.append("Generic vector fallback dominates; fast paths are not wired.")
    if f["chunks"] > 0:
        signals.append(f"flat_chunks={int(f['flat_chunks'])}, dict_chunks={int(f['dict_chunks'])}, generic_chunks={int(f['generic_chunks'])}")
        if f["generic_chunks"] > 0:
            findings.append(f"generic_fallback_chunks={int(f['generic_chunks'])} ({100.0 * f['generic_chunks'] / f['chunks']:.1f}% of {int(f['chunks'])} chunks).")
    if f["pre_rows"] > 0 and f["distinct_inserts"] > 0:
        findings.append(f"distinct_inserts={int(f['distinct_inserts'])} ({f['distinct_inserts'] / f['pre_rows']:.1%} of {int(f['pre_rows'])} rows).")
    if f["pre_rows"] > 0 and f["qualifying_rows"] > 0 and f["pre_rows"] / f["qualifying_rows"] >= 5:
        amplification = f["pre_rows"] / f["qualifying_rows"]
        findings.append(f"scan_amplification={amplification:.1f}x (pre_rows={int(f['pre_rows']):,}, qualifying_rows={int(f['qualifying_rows']):,}).")
        signals.append(f"scan_amplification={amplification:.1f}x (pre_rows / qualifying_rows)")
        ask("Many scanned rows never reach aggregation — what predicates or joins reject them, "
            "and can any filter move earlier (NativeScanSpec, runtime TableFilterSet, scan order)?")
    if f["num_scan_us"] > 0 and f["aggregate_us"] > 0:
        share = f["aggregate_us"] / f["num_scan_us"]
        findings.append(f"aggregate_us={f['aggregate_us']:.0f} ({share:.0%} of num_scan_us={f['num_scan_us']:.0f}).")
        signals.append(f"num_aggregate_us={f['aggregate_us']:,.0f} ({share:.0%} of num_scan_us={f['num_scan_us']:,.0f})")
        ask("What does `num_aggregate_us` include — only qualifying rows or the full consume loop "
            "(including rejected probes)? Trace timers around ProcessNum* / Accumulate.")
    elif f["num_scan_us"] > 0:
        if f["qualifying_rows"] > 10_000:
            findings.append("Fused scan: num_scan_us likely includes per-row aggregation (aggregate_us not reported separately).")
        if f["qualifying_rows"] > 1_000:
            signals.append("num_scan_us reported without separate num_aggregate_us")
            ask("Fused scan may bundle consume into scan timing — grep the production path to separate I/O from per-row work.")
    if f["finalize_us"] > 0 and f["pre_scan_us"] > 0 and f["finalize_us"] / f["pre_scan_us"] >= 0.15:
        signals.append(f"finalize_us={f['finalize_us']:,.0f} ({f['finalize_us'] / f['pre_scan_us']:.0%} of pre_scan_us)")
    if f["sub_scan_us"] > 0 and f["pre_scan_us"] > 0:
        signals.append(f"sub_scan_us={f['sub_scan_us']:,.0f}")
    if speedup < max(best_speedup, 1.0) and has_profile:
        ask("Ship a coordinated bundle (2–4 related changes) per A/B trial — micro-opts alone won't close a large gap.")
    high = [s for s in smells if s.get("severity") == "high"]
    if high:
        findings.append(f"static_kernel_review: {len(high)} high-severity smell(s) on file.")
        signals += [f"static_review: {s.get('message') or s.get('id') or 'structure smell'}" for s in high]
        ask("Static review flagged a smell — confirm by reading the code whether it is on the hot path (not build-only code).")
    if not phases and speedup < 1.0:
        lacking.append("No profile phase breakdown available — profile gate may not have run or counters are zero.")
    if not questions:
        ask("Trace the production path (fused prescan → consume → aggregate); "
            "identify the dominant per-row work and verify by reading the code before editing.")

    menu = _technique_menu(f, dominant, smells, speedup) if has_profile else [
        "benchmark_profile: ensure benchmark emits profile CSV (ACCEL_BENCH_PROFILE=1 on fused production path)",
        f"fused_production_path: {TECHNIQUES['fused_production_path']}"]
    plan = (analysis or {}).get("logical_plan") or {}
    leads = []
    if simplest := plan.get("simplest_execution_strategy") or ((analysis or {}).get("query_summary") or {}).get("pattern"):
        leads.append(f"analysis_plan: {simplest}")
    if op := next((o for o in plan.get("operators") or [] if isinstance(o, str) and o.strip()), None):
        leads.append(f"analysis_operator: {op.strip()}")
    menu = [*(x for x in leads if x not in menu), *menu]
    focus = ([lacking[0]] if lacking else []) + ([f"Try coordinated bundle from palette: {menu[0]}"] if menu else []) + (
        [f"Dominant phase `{dominant}` — verify the inner loop in the code before editing"] if dominant else [])
    return {
        "speedup_median": speedup, "best_speedup_median": best_speedup, "beats_duckdb": speedup >= 1.0,
        "regressed_vs_best": regressed, "reference_median_ms": ref_ms, "accelerator_median_ms": acc_ms,
        "dominant_phase": dominant, "dominant_phase_us": dominant_us, "findings": findings, "lacking": lacking,
        "profile_signals": signals, "investigation_questions": questions[:6], "strategy_palette": menu,
        "suggested_focus": focus[:4], "profile_counters": counters, "profile_phases_us": phases, "kernel_review": kernel_review or {},
    }


def bottleneck_evidence(
    diagnosis: dict[str, Any], profile: dict[str, Any], kernel_review: dict[str, Any] | None = None,
    *, analysis: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """The measured evidence organized by structural tier (it does not rank or prescribe fixes)."""
    tiers: dict[str, dict[str, Any]] = {t: {"tier": t, "signals": [], "relevant_phases_us": {}, "suggested_grep_targets": []} for t in TIERS}
    phases = dict(diagnosis.get("profile_phases_us") or {})
    counters = dict(diagnosis.get("profile_counters") or {})
    total = sum(float(v) for v in phases.values() if v) or 0.0
    bound = lambda us: round(100.0 * us / total, 1) if total > 0 and us > 0 else None  # noqa: E731

    dominant, dominant_us = diagnosis.get("dominant_phase"), float(diagnosis.get("dominant_phase_us") or 0.0)
    if dominant and dominant_us > 0:
        entry = tiers[_tier_for_signal(str(dominant))]
        entry["signals"].append({"metric": str(dominant), "value": dominant_us,
                                 "context": f"dominant phase ({100.0 * dominant_us / max(total, 1):.0f}% of profiled path)"})
        entry["relevant_phases_us"][str(dominant)] = dominant_us
        if (b := bound(dominant_us)) is not None:
            entry["upper_bound_pct"] = b
    pre_rows, generic_rows, qualifying = (int(counters.get(k) or 0) for k in ("pre_rows", "generic_rows", "qualifying_rows"))
    if generic_rows > 0 and pre_rows > 0:
        tiers["reduce_decode_materialize"]["signals"].append(
            {"metric": "generic_rows", "value": generic_rows, "context": f"{100.0 * generic_rows / pre_rows:.1f}% of pre_rows={pre_rows}"})
    if pre_rows > 0 and qualifying > 0 and pre_rows / qualifying >= 3:
        tiers["exploit_selectivity"]["signals"].append(
            {"metric": "scan_amplification", "value": round(pre_rows / qualifying, 2), "context": f"pre_rows={pre_rows}, qualifying_rows={qualifying}"})
    for signal in diagnosis.get("profile_signals") or []:
        tiers[_tier_for_signal(str(signal))]["signals"].append({"metric": "profile_signal", "value": str(signal), "context": ""})
    kernel_path = (kernel_review or {}).get("kernel_path", "")
    for smell in (kernel_review or {}).get("smells") or []:
        tier = tiers[_tier_for_signal(str(smell.get("id", "")))]
        tier["signals"].append({"metric": str(smell.get("id", "kernel_smell")), "value": smell.get("severity", ""),
                                "context": str(smell.get("message", ""))[:120]})
        if smell.get("line") and kernel_path:
            tier["suggested_grep_targets"].append(f"{kernel_path}:{smell['line']}")
    for item in ((analysis or {}).get("likely_bottlenecks") or [])[:3]:
        tiers["eliminate_unnecessary_work"]["signals"].append({"metric": "analysis_bottleneck", "value": str(item), "context": "from analysis.json"})
    present = [tiers[t] for t in TIERS if tiers[t]["signals"]]
    for entry in present:
        if "upper_bound_pct" not in entry and (b := bound(sum(float(v) for v in entry["relevant_phases_us"].values()))) is not None:
            entry["upper_bound_pct"] = b
    return {"tiers": present, "speedup_median": diagnosis.get("speedup_median"), "dominant_phase": dominant,
            "profile_counters": counters, "profile_phases_us": phases, "findings": (diagnosis.get("findings") or [])[:5],
            "lacking": (diagnosis.get("lacking") or [])[:4], "profile": profile}


def _pick_metrics(art: Path) -> tuple[dict[str, Any], str]:
    """The freshest measured aux metrics on disk, and the file they came from."""
    profile = read_json(art / "profile_summary.json")
    if profile.get("workload_kind") == "aux_state" and (profile.get("store_diagnostics") or profile.get("read_only_query_speedup")):
        return profile, "profile_summary.json"
    if best := read_json(art / BEST_VALID_FILE):
        return best_valid_metrics(best), BEST_VALID_FILE
    if fair := read_json(art / "aux_bench_summary.json"):
        return fair, "aux_bench_summary.json"
    if best_summary := read_json(art / "optimization_state.json").get("best_summary"):
        return dict(best_summary), "optimization_state.json"
    return {}, ""


def aux_profile_digest(art: Path, *, repo_root: Path | None = None, editable_rel_paths: list[str] | None = None) -> dict[str, Any]:
    """The aux-state accelerator's profile: scoreboard, freshness, phases, diagnostics, the best states on record."""
    metrics, source = _pick_metrics(art)
    events = list(read_json(art / "custom_profile.json").get("events") or read_json(art / "profile_summary.json").get("custom_profile_events") or [])
    summary = build_aux_profile_summary(metrics, source=source or "artifacts", custom_events=events) if metrics else {
        "workload_kind": "aux_state", "source": "missing", "store_diagnostics": {}, "custom_profile_events": events, "phase_breakdown": [],
        "profile_signals": ["no aux metrics on disk — call run_profile or run_quick_benchmark"]}
    board = build_scoreboard(art, is_aux=True, last_metrics=metrics or None)
    write_scoreboard(art, board)
    fair, state, best = read_json(art / "aux_bench_summary.json"), read_json(art / "optimization_state.json"), read_json(art / BEST_VALID_FILE)
    kernel = read_json(art / "best_kernel_snapshot" / "manifest.json")
    return {
        "workload_kind": "aux_state",
        "scoreboard": board,
        "freshness": profile_freshness(art, repo_root=repo_root, editable_rel_paths=editable_rel_paths),
        "profile_summary": summary,
        "phase_breakdown": summary.get("phase_breakdown") or [],
        "work_counters_mean": summary.get("work_counters_mean") or {},
        "residual_vs_hit_us": summary.get("residual_vs_hit_us"),
        "baseline_summary": read_json(art / "baseline_summary.json"),
        "store_diagnostics": summary.get("store_diagnostics") or {},
        "fair_sweep": {"read_only_query_speedup": fair.get("read_only_query_speedup"), "composite_score": fair.get("composite_score"),
                       "accel_hit_median_us": ((fair.get("by_write_frequency_pct") or {}).get("0") or {}).get("accel_hit_median_us")} if fair else {},
        "optimization_state_best": {"best_composite_score": state.get("best_composite_score"),
                                    "read_only_query_speedup": (state.get("best_summary") or {}).get("read_only_query_speedup")} if state else {},
        "best_valid_baseline": {"score": best.get("score"), "attempt_id": best.get("attempt_id"),
                                "accel_hit_median_us": (best.get("metrics") or {}).get("accel_hit_median_us")} if best else {},
        "best_kernel_snapshot": {"speedup_median": kernel.get("speedup_median"), "change_kind": kernel.get("change_kind")} if kernel else {},
        "custom_profiling": custom_profiling_contract(is_aux=True),
        "measurement_rules": [
            "Call run_profile on the CURRENT tree before forming a hypothesis.",
            "If freshness.stale is true, do not hypothesize from this digest — remeasure.",
            "Cite dominant_phase, phase_breakdown means, and work_counters_mean by name/value.",
            "A/B keep/regress vs scoreboard.best_valid_score only.",
            "After accept/restore, profile is stale until run_profile runs again.",
        ],
    }


def _aux_advice(art: Path, pipeline_config: PipelineConfig, digest: dict[str, Any]) -> dict[str, Any]:
    """The aux-state accelerator's evidence tiers, focus and gaps (the scoreboard and profile come from `digest`)."""
    profile, board = dict(digest.get("profile_summary") or {}), dict(digest.get("scoreboard") or {})
    best_valid, fair = read_json(art / BEST_VALID_FILE), read_json(art / "aux_bench_summary.json")
    best_metrics = dict(best_valid.get("metrics") or {})
    floor = early_stop_config(pipeline_config)["saturation_min_read_speedup"]
    best_read = float(best_metrics.get("read_only_query_speedup")
                      or (read_json(art / "optimization_state.json").get("best_summary") or {}).get("read_only_query_speedup")
                      or fair.get("read_only_query_speedup") or profile.get("read_only_query_speedup") or 0.0)
    last_read = float(profile.get("read_only_query_speedup") or best_read or 0.0)
    diag, phases = dict(profile.get("store_diagnostics") or {}), dict(profile.get("profile_phases_us") or {})
    by_freq = {**dict(fair.get("by_write_frequency_pct") or {}), **dict(best_metrics.get("by_write_frequency_pct") or {})}
    row = by_freq.get("10") or by_freq.get("1") or {}
    write_mix = float(best_metrics.get("write_mix_workload_speedup") or row.get("workload_speedup") or 0.0)
    mix_pct = int(best_metrics.get("write_mix_pct") or row.get("write_frequency_pct") or (10 if write_mix > 0 else 0))
    tiers = [{"tier": "aux_read_path", "relevant_phases_us": phases,
              "signals": [{"metric": k, "value": v, "context": "store_diagnostics"} for k, v in diag.items()][:8]}]
    if mix_pct > 0:
        tiers.insert(0, {"tier": "aux_write_maintenance", "signals": [
            {"metric": "write_mix_workload_speedup", "value": write_mix, "context": f"{mix_pct}% write quick-bench / fair sweep"},
            {"metric": "guidance", "value": "Prefer ApplyDelta/partition-local maintenance; RebuildAll-per-point-write is a cliff", "context": "dual_axis"}]})
    if profile.get("custom_profile_events"):
        tiers.append({"tier": "custom_instrumentation", "signals": [
            {"metric": "custom_profile_events", "value": len(profile["custom_profile_events"]), "context": "ACCEL_CUSTOM_PROFILE lines"}]})
    cliff = 0 < write_mix < 1.0
    best_score = float(board.get("best_valid_score") or 0.0)
    focus = [
        f"Accept score = read^0.75 * write_mix^0.25" + (f" (best_valid={best_score:.3f})" if best_score > 0 else "")
        + f"; read_only best={best_read:.3f}x; {mix_pct or 10}% write-mix" + (f"={write_mix:.2f}x" if write_mix > 0 else " (measure via quick bench)."),
        "Cheap incremental maintenance is required — Prefer ApplyDelta / touched partitions over RebuildAll on every point write. "
        "Raising write-mix raises the accept score.",
        "Designs must scale with SF (bind-time row estimates; no hardcoded iteration-database sizes).",
        "Measure-first: run_profile on CURRENT tree; cite phase_breakdown + 0% and 10% metrics.",
    ]
    if cliff and best_read >= floor:
        focus.insert(0, f"PRIORITY: write-mix cliff ({mix_pct}% workload={write_mix:.2f}x ≪ 1×) while read is already strong "
                        f"({best_read:.1f}x) — hypothesize maintenance (ApplyDelta / local rebuild), not further hit-path micro-opts.")
    if float(diag.get("accel_hit_median_us") or 0) > 0:
        focus.append(f"Hit path currently {float(diag['accel_hit_median_us']):.0f}µs @ hit_rate={float(diag.get('accel_hit_rate') or 0):.2f}.")
    if profile.get("dominant_phase"):
        focus.append(f"Dominant timed phase: {profile['dominant_phase']} @ {float(profile.get('dominant_phase_us') or 0):.1f}µs.")
    freshness, fair_read = digest.get("freshness") or {}, float(fair.get("read_only_query_speedup") or 0.0)
    lacking = [f"STALE PROFILE: {r}" for r in list(freshness.get("reasons") or [])[:3]] if freshness.get("stale") else []
    if fair_read > 0 and last_read + 0.05 < fair_read:
        lacking.append(f"Quick/current read_only {last_read:.3f}x is below fair-sweep best {fair_read:.3f}x — restore best_kernel before chasing micro-opts.")
    if cliff:
        lacking.append(f"WRITE-MIX CLIFF: {mix_pct}% workload_speedup={write_mix:.2f}x — incremental maintenance too expensive "
                       "(likely full rebuild per write). Attack ApplyDelta / partition-local paths before more read micro-opts.")
    if not profile.get("phase_breakdown"):
        lacking.append("No split phase timers yet — add ACCEL_CUSTOM_PROFILE phase+us+counts, then run_profile before hypothesizing.")
    lacking.append("Expect large gains on BOTH axes when aux is valid; micro-shuffles that move <5% on read while leaving a write cliff are the wrong tier.")
    return {
        "best": best_read, "last": last_read, "current": last_read, "regressed": best_read > 0 and last_read + 0.01 < best_read,
        "read_floor": floor, "fair_sweep_read_only": fair_read, "write_mix_pct": mix_pct, "write_mix_workload_speedup": write_mix,
        "profile_lines": [f"- {s}" for s in profile.get("profile_signals") or ["call MCP run_profile / run_quick_benchmark to refresh aux metrics"]],
        "questions": ["Where does accel_hit_median_us go (decode vs scan vs materialize vs join)?",
                      "Is StageMaintenance RebuildAll-only, or is ApplyDelta / partition-local used for point writes?",
                      "What dominates 10% write-mix time: rebuild, delta apply, or cold build amortized?",
                      "Can partition layout / encoding cut partitions_read or state_bytes_read at SF20+?"],
        "evidence": {"tiers": tiers}, "focus": focus[:4], "lacking": lacking[:5], "findings": [], "palette": [], "recent_iterations": [],
    }


def _base_advice(art: Path, kernel_review: dict[str, Any], analysis: dict[str, Any] | None) -> dict[str, Any]:
    state = read_json(art / "optimization_state.json")
    baseline, profile = read_json(art / "baseline_summary.json"), read_json(art / "profile_summary.json")
    best = float(state.get("best_speedup_median") or 0.0)
    diagnosis = diagnose(baseline, profile, best_speedup=best, analysis=analysis, kernel_review=kernel_review)
    phases, counters = diagnosis["profile_phases_us"], diagnosis["profile_counters"]
    lines = [f"- {s}" for s in diagnosis["profile_signals"][:8]]
    lines += [f"- `{k}={counters[k]}`" for k in ("flat_chunks", "dict_chunks", "generic_chunks") if counters.get(k) is not None]
    lines += [f"- `{k}={float(phases[k]):.0f}`" for k in ("sub_scan_us", "num_scan_us", "num_aggregate_us", "pre_consume_us", "finalize_us") if phases.get(k) is not None]
    return {
        "best": state.get("best_speedup_median"), "last": state.get("last_speedup_median"), "current": diagnosis["speedup_median"],
        "regressed": diagnosis["regressed_vs_best"], "profile_lines": lines, "questions": diagnosis["investigation_questions"],
        "evidence": bottleneck_evidence(diagnosis, profile, kernel_review, analysis=analysis),
        "focus": diagnosis["suggested_focus"][:2], "lacking": diagnosis["lacking"][:4], "findings": diagnosis["findings"][:5],
        "palette": diagnosis["strategy_palette"], "recent_iterations": (state.get("iterations") or [])[-2:],
        "profile_counters": counters, "diagnosis": diagnosis,
        "latency_ms": (diagnosis["reference_median_ms"], diagnosis["accelerator_median_ms"]),
    }


def advise(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *, optimization_iter: int, episode: int = 1) -> dict[str, Any]:
    """The digest for the agent: the measurements on disk, the static review of the kernel, and what earlier
    attempts and the engine's lessons say about evidence like this."""
    repo = pipeline_config.repo_root
    art = query_artifacts_dir(repo, query, workload=workload)
    is_aux = workload.get("workload_kind") == "aux_state"
    _, analysis = load_query_analysis(repo, query, workload=workload)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
    review = review_kernel(repo / edit["primary_cpp"])
    digest = aux_profile_digest(art, repo_root=repo, editable_rel_paths=[p for p in edit.get("all_paths") or [] if p]) if is_aux else {}
    advice = _aux_advice(art, pipeline_config, digest) if is_aux else _base_advice(art, review, analysis or None)
    metrics = (digest.get("profile_summary") or {}) if is_aux else read_json(art / "profile_summary.json")
    workflow = repo / "pipeline/prompts/shared/optimization_workflow_compact.md"
    return {
        **advice, "optimization_iter": optimization_iter, "episode": episode, "query": query, "workload_kind": workload.get("workload_kind"),
        "scoreboard": digest.get("scoreboard") or {}, "freshness": digest.get("freshness") or {},
        "phase_breakdown": digest.get("phase_breakdown") or [], "work_counters_mean": digest.get("work_counters_mean") or {},
        "residual_vs_hit_us": digest.get("residual_vs_hit_us"), "store_diagnostics": digest.get("store_diagnostics") or {},
        "kernel_guidance": kernel_review_lines(review),
        "attempts": recall(art, "attempt", limit=5), "techniques": recall(art, "technique", limit=8),
        "lessons": lessons(repo, pipeline_config.engine_name, evidence_signals(art, metrics, analysis or None), limit=8),
        "workflow": workflow.read_text(encoding="utf-8").strip() if workflow.is_file() else "",
        "kernel_paths": {
            "header": edit["header"], "kernel": edit["primary_cpp"], "package": edit["secondary_cpp"],
            "fused_scan": None if is_aux else paths.get("fused_scan_path"), "cost_model": paths.get("cost_model_config") if is_aux else None,
        },
    }


def _read_axis_lines(board: dict[str, Any], floor: float) -> list[str]:
    axes = dict((board.get("read_speedup") or {}).get("best_valid") or {})
    last = dict((board.get("read_speedup") or {}).get("last_measured") or {})
    fmt = lambda value: f"{float(value):.2f}x" if value else "n/a"  # noqa: E731
    lines = [f"- **READ AXIS (best valid)**: {fmt(axes.get('vs_engine'))} vs the engine's own SQL",
             f"- read axis (last measured): {fmt(last.get('vs_engine'))} vs the engine's own SQL"]
    if floor > 0 and axes.get("vs_engine") and float(axes["vs_engine"]) < floor:
        lines.append(f"- **READ FLOOR NOT MET** ({float(axes['vs_engine']):.2f}x < {floor:g}x over the engine's SQL): the write axis "
                     "does not excuse this. Profile first, then attack the top hot spot; restructure when the "
                     "profile shows the read visits too much state.")
    return lines


def render_digest(state: dict[str, Any]) -> str:
    is_aux = state.get("workload_kind") == "aux_state"
    label = "read_only_query_speedup" if is_aux else "speedup_median"
    board = state.get("scoreboard") or {}
    x = lambda key: f"{float(state.get(key) or 0):.3f}x"  # noqa: E731
    lines = [
        f"# Optimization runtime (iter {state['optimization_iter']}, episode {state['episode']})", "",
        "## Scoreboard (stable — A/B compare against best_valid only)",
        f"- score_metric: `{board.get('score_metric') or ('dual_axis_weighted(read^0.75 * write_mix^0.25)' if is_aux else 'speedup_median')}`",
        f"- best_{label}: **{x('best')}**", f"- last_{label}: **{x('last')}**", f"- last_gate {label}: **{x('current')}**",
    ]
    if not is_aux and all(state.get("latency_ms") or (None, None)):
        lines.append("- latency: DuckDB≈%.1fms, accelerator≈%.1fms" % tuple(state["latency_ms"]))
    if board.get("best_valid_score") is not None:
        lines.append(f"- scoreboard.best_valid_score: **{float(board['best_valid_score']):.3f}x** "
                     f"(attempt={board.get('best_valid_attempt_id') or 'n/a'}; hit_us={board.get('best_valid_hit_us')})")
    if board.get("improvement_pct_vs_best_valid") is not None:
        lines.append(f"- last vs best_valid: **{float(board['improvement_pct_vs_best_valid']):+.2f}%**")
    if is_aux and state.get("fair_sweep_read_only"):
        lines.append(f"- fair_sweep_read_only: **{float(state['fair_sweep_read_only']):.3f}x** "
                     "(iteration-database fair reference — do not confuse with A/B keep/regress target)")
    if is_aux and state.get("write_mix_pct"):
        wm = float(state.get("write_mix_workload_speedup") or 0.0)
        lines.append(f"- write_mix ({int(state['write_mix_pct'])}%): **{wm:.3f}x** workload (feeds dual-axis accept score as write_mix^0.25)")
        if 0 < wm < 1.0:
            lines.append("- **WRITE-MIX CLIFF** — prefer ApplyDelta / local rebuilds; do not spend the attempt on tiny read micro-opts while this stays ≪1×.")
    if is_aux:
        lines += _read_axis_lines(board, float(state.get("read_floor") or 0.0))
    lines.append("- Early stop: `OPTIMIZATION_SATURATED` is honored once the read speedup over the engine's own SQL reaches the floor; "
                 "below it, only after the last trials were each profile-guided (`begin_ab_trial` with hot_spot, expected_gain, "
                 "strategy), distinct, and included one `structural` and one `low_level` change.")
    if state.get("regressed"):
        lines.append("- **REGRESSION vs best** — workspace should be on restored best snapshot.")
    if freshness := state.get("freshness"):
        lines += ["", "## Profile freshness", f"- stale: **{bool(freshness.get('stale'))}**", *(f"- {r}" for r in list(freshness.get("reasons") or [])[:4])]
    lines += ["", "## Profile signals", *(state.get("profile_lines") or ["- (no profile data yet — run `run_quick_benchmark` / `run_profile`)"])]
    if rows := state.get("phase_breakdown"):
        lines += ["", "## Phase breakdown (custom timers)", "| phase | mean_us | p50_us | n | counts_mean |", "|-------|---------|--------|---|-------------|"]
        for row in rows[:12]:
            counts = ",".join(f"{k}={v}" for k, v in list((row.get("counts_mean") or {}).items())[:4]) or "-"
            lines.append(f"| {row.get('phase')} | {row.get('mean_us')} | {row.get('p50_us')} | {row.get('n')} | {counts} |")
        if state.get("residual_vs_hit_us") is not None:
            lines.append(f"- residual_vs_hit_us: {state['residual_vs_hit_us']}")
    if work := state.get("work_counters_mean"):
        lines += ["", "## Work counters (mean)", *(f"- `{k}={v}`" for k, v in list(work.items())[:10])]
    if is_aux and state.get("store_diagnostics"):
        lines += ["", "## Store diagnostics", *(f"- `{k}={v}`" for k, v in state["store_diagnostics"].items())]
    if state.get("findings"):
        lines += ["", "## Profile findings", *(f"- {item}" for item in state["findings"][:4])]
    if state.get("questions"):
        lines += ["", "## Investigate (read the code before editing)", *(f"- {item}" for item in state["questions"])]
    if state.get("kernel_guidance"):
        lines += ["", *state["kernel_guidance"]]
    if tiers := (state.get("evidence") or {}).get("tiers"):
        lines += ["", "## Bottleneck evidence (tier-organized — pick ONE hypothesis)"]
        for block in tiers[:7]:
            lines.append(f"### {block.get('tier', '')}")
            lines += [f"- {s.get('metric')}: {s.get('value')} ({s.get('context', '')})" for s in (block.get("signals") or [])[:4]]
            if block.get("upper_bound_pct") is not None:
                lines.append(f"- upper_bound_pct (Amdahl): {block['upper_bound_pct']}")
    if state.get("palette"):
        lines += ["", "## Technique palette (pick a coordinated bundle)", *(f"{i}. {item}" for i, item in enumerate(state["palette"][:6], 1))]
    if state.get("focus"):
        lines += ["", "## Focus this iteration", *(f"- {item}" for item in state["focus"][:3])]
    lines += ["", "## Attempt history (this optimization iter)"]
    if attempts := state.get("attempts"):
        lines += ["| attempt | hypothesis | improvement | kept |", "|---------|------------|-------------|------|"]
        lines += [f"| {a.get('attempt', '')} | {_clip(a.get('hypothesis'), 48)} | {a.get('improvement_pct', 'n/a')} | {a.get('kept', a.get('accepted', False))} |" for a in attempts]
    else:
        lines.append("- No attempts yet.")
    if state.get("lacking"):
        lines += ["", "## Gaps vs goal", *(f"- {item}" for item in state["lacking"])]
    if state.get("recent_iterations") and not is_aux:
        lines += ["", "## Recent gate iterations"]
        lines += [f"- iter {r.get('iteration')}: {float(r.get('speedup_median') or 0):.3f}x ({'improved' if r.get('improved') else 'flat/regressed'})"
                  for r in state["recent_iterations"]]
    if techniques := state.get("techniques"):
        lines += ["", "## Technique memory (structured)"]
        for row in techniques:
            verdict = "kept" if row.get("kept") else "discarded" if row.get("kept") is False else "n/a"
            delta = f", Δspeedup={float(row['speedup_delta']):+.3f}x" if row.get("speedup_delta") is not None else ""
            bits = ", ".join(f"{k}={v}" for k, v in list((row.get("profile_deltas") or {}).items())[:3])
            lines.append(f"- iter {row.get('iteration', '?')} `{row.get('technique_id', 'unknown')}` ({verdict}{delta}{', ' + bits if bits else ''})")
    if note := lessons_markdown(state.get("lessons") or []):
        lines += ["", *note]
    if state.get("workflow"):
        lines += ["", "## Optimization workflow", state["workflow"]]
    lines += ["", "## Scale invariance",
              "- Benchmark/profile are one dataset size only; optimizations must scale with row volume.",
              "- Size structures from bind-time `row_estimate` / table row counts — not hardcoded profile counters."]
    if is_aux:
        lines.append("- Custom profiling: getenv(`ACCEL_AUX_CUSTOM_PROFILE`) + `ACCEL_CUSTOM_PROFILE {json}` lines are allowed; no-op when unset.")
    if pre_rows := int((state.get("profile_counters") or {}).get("pre_rows") or 0):
        lines.append(f"- Current `pre_rows` (this run only): {pre_rows} — do not bake this constant into code.")
    lines.append("")
    lines += [f"- {key}: `{value}`" for key, value in (state.get("kernel_paths") or {}).items() if value]
    return "\n".join(lines)


def refresh_optimization_runtime(
    pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *, optimization_iter: int, episode: int = 1
) -> Path:
    """Re-read the measurements and write the digest the agent reads (`optimization_runtime.md`, and its json)."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    state = advise(pipeline_config, workload, query, optimization_iter=optimization_iter, episode=episode)
    (art / "optimization_runtime.json").write_text(json.dumps(state, indent=2, default=str), encoding="utf-8")
    (art / "optimization_runtime.md").write_text(render_digest(state), encoding="utf-8")
    return art / "optimization_runtime.md"


# The agent's digest tools

_DIGEST_MAX_CHARS = 50_000
_EVIDENCE_MAX_CHARS = 80_000


def digest_tools(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int):
    """`get_profile_digest`, `get_optimization_evidence` and `get_store_diagnostics`, as callables."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    is_aux = workload.get("workload_kind") == "aux_state"

    def get_profile_digest() -> str:
        """Compact phase counters / dominant bottleneck from last profile artifacts."""
        editable = [p for p in resolve_optimization_edit_paths(pipeline_config, workload, query).get("all_paths") or [] if p]
        if is_aux:
            payload = aux_profile_digest(art, repo_root=pipeline_config.repo_root, editable_rel_paths=editable)
        else:
            profile, baseline = read_json(art / "profile_summary.json"), read_json(art / "baseline_summary.json")
            board = build_scoreboard(art, is_aux=False, last_metrics=profile or baseline)
            write_scoreboard(art, board)
            payload = {
                "scoreboard": board,
                "freshness": profile_freshness(art, repo_root=pipeline_config.repo_root, editable_rel_paths=editable),
                "profile_summary": profile, "baseline_summary": baseline, "custom_profiling": custom_profiling_contract(is_aux=False),
                "measurement_rules": ["Call run_profile on the CURRENT tree before forming a hypothesis.",
                                      "If freshness.stale is true, do not hypothesize from this digest — remeasure.",
                                      "A/B keep/regress vs scoreboard.best_valid_score only."],
            }
        runtime = art / "optimization_runtime.md"
        payload["runtime_digest_tail"] = runtime.read_text(encoding="utf-8")[-4000:] if runtime.is_file() else ""
        return json.dumps(payload, indent=2)[:_DIGEST_MAX_CHARS]

    def get_optimization_evidence() -> str:
        """The runtime digest (and, for aux, a fresh profile digest, so an empty runtime still helps)."""
        runtime = art / "optimization_runtime.md"
        parts = [runtime.read_text(encoding="utf-8")[:40_000]] if runtime.is_file() else []
        if is_aux or not parts:
            parts.append(get_profile_digest())
        return "\n\n".join(parts)[:_EVIDENCE_MAX_CHARS]

    def get_store_diagnostics() -> str:
        """Store diagnostics from bench/opt state when present."""
        names = ("profile_summary.json", BEST_VALID_FILE, "aux_bench_summary.json", "optimization_state.json", "baseline_summary.json", "bench_summary.json")
        for name in names if is_aux else ("bench_summary.json", "optimization_state.json", "baseline_summary.json"):
            data = read_json(art / name)
            data = best_valid_metrics(data) if name == BEST_VALID_FILE else dict(data.get("best_summary") or {}) if name == "optimization_state.json" and is_aux else data
            nested = data.get("store_diagnostics") if isinstance(data.get("store_diagnostics"), dict) else data.get("median_metrics", {}).get("store_diagnostics")
            diag = (extract_store_diagnostics(data) or extract_store_diagnostics(nested or {})) if is_aux else nested
            if diag:
                return json.dumps({"source": name, "store_diagnostics": diag}, indent=2)
        note = "no aux diagnostics yet — call run_profile or run_quick_benchmark" if is_aux else "no diagnostics on disk yet"
        return json.dumps({"store_diagnostics": {}, "note": note})

    return {"get_profile_digest": get_profile_digest, "get_optimization_evidence": get_optimization_evidence,
            "get_store_diagnostics": get_store_diagnostics}
