"""Resolve aux optimization quick-bench write frequencies (incl. random write-mix).

Static ``write_frequencies: [0, 10]`` lets agents overfit maintenance to a single
mix. Optional ``write_mix_sample`` draws additional write % points from a pool.
Frequencies are locked **per attempt** (so multi-sample benches within one attempt
share a load) and **re-sampled every attempt**. ``0%`` is always included.
"""

from __future__ import annotations

import json
import math
import os
import random
from pathlib import Path
from typing import Any

from accel_pipeline.paths import find_repo_root
from accel_pipeline.test_suite import draw_write_frequencies, read_write_mix, suite_path

LOCK_FILENAME = "optimization_write_frequencies.json"

_DEFAULT_WRITE_MIX_CHOICES = (1, 5, 10, 15, 20, 25, 33, 40, 50)


def _parse_freq_list(raw: Any) -> list[int]:
    if isinstance(raw, list):
        return [int(x) for x in raw]
    if isinstance(raw, str) and raw.strip():
        return [int(part.strip()) for part in raw.split(",") if part.strip()]
    return []


def _opt_sweep(workload: dict[str, Any]) -> dict[str, Any]:
    return dict(workload.get("aux_bench_sweep_optimization") or {})


def _sample_cfg(workload: dict[str, Any]) -> dict[str, Any]:
    return dict(_opt_sweep(workload).get("write_mix_sample") or {})


def base_write_frequencies(workload: dict[str, Any]) -> list[int]:
    """Configured static frequencies (optimization override, else fair sweep, else default)."""
    opt_sweep = _opt_sweep(workload)
    sweep = workload.get("aux_bench_sweep") or {}
    raw = opt_sweep.get("write_frequencies")
    if raw is None:
        raw = sweep.get("write_frequencies")
    freqs = _parse_freq_list(raw)
    if not freqs:
        freqs = [0, 1, 10, 50]
    seen: set[int] = set()
    out: list[int] = []
    for f in freqs:
        if f not in seen:
            seen.add(f)
            out.append(int(f))
    return out


def sample_write_mix_enabled(workload: dict[str, Any]) -> bool:
    cfg = _sample_cfg(workload)
    if "enabled" in cfg:
        return bool(cfg["enabled"])
    return bool(cfg.get("choices") or cfg.get("count"))


def lock_per_attempt_enabled(workload: dict[str, Any]) -> bool:
    cfg = _sample_cfg(workload)
    if "lock_per_attempt" in cfg:
        return bool(cfg["lock_per_attempt"])
    return True


def _suite_write_mix(workload: dict[str, Any]) -> dict[str, Any] | None:
    """The query's own write-frequency distribution (its test suite's write_mix.yaml), capped at the
    optimization bench's `max_pct`; None when the query has no suite."""
    try:
        path = suite_path(find_repo_root(), str(workload.get("benchmark_name", "tpch")), int(workload["query_number"]))
        mix = read_write_mix(path)
    except (FileNotFoundError, KeyError, ValueError):
        return None
    spec = dict(mix["frequency_pct"])
    cap = _sample_cfg(workload).get("max_pct")
    if cap is not None:
        spec["max"] = max(int(spec["min"]), min(int(spec["max"]), int(cap)))
    return {**mix, "frequency_pct": spec}


def _sample_write_mix_freqs(
    workload: dict[str, Any],
    *,
    already: list[int],
    rng: random.Random,
) -> list[int]:
    cfg = _sample_cfg(workload)
    count = max(0, int(cfg.get("count", 1)))
    if count <= 0:
        return []
    if mix := _suite_write_mix(workload):
        return [f for f in draw_write_frequencies(mix, count, rng.getrandbits(31))[1:] if f not in already]
    choices_raw = cfg.get("choices")
    if choices_raw is None:
        choices = list(_DEFAULT_WRITE_MIX_CHOICES)
    else:
        choices = [int(x) for x in _parse_freq_list(choices_raw)]
    cap = int(cfg.get("max_pct") or max(choices))
    pool = [c for c in choices if 0 < c <= cap and c not in already]
    if not pool:
        return []
    k = min(count, len(pool))
    return sorted(rng.sample(pool, k=k))


def _merge_freqs(base: list[int], extra: list[int]) -> list[int]:
    seen: set[int] = set()
    out: list[int] = []
    for f in [*base, *extra]:
        fi = int(f)
        if fi not in seen:
            seen.add(fi)
            out.append(fi)
    if 0 not in seen:
        out.insert(0, 0)
    return out


def _lock_path(art_dir: Path) -> Path:
    return art_dir / LOCK_FILENAME


def _rng_for_sample(
    workload: dict[str, Any],
    *,
    optimization_iter: int | None,
    attempt: int | None,
    rng: random.Random | None,
) -> random.Random:
    cfg = _sample_cfg(workload)
    seed_raw = cfg.get("seed")
    if seed_raw is not None:
        # Distinct draws per attempt when a fixed seed is configured.
        base = int(seed_raw)
        mix = base
        if optimization_iter is not None:
            mix ^= int(optimization_iter) * 0x9E3779B1
        if attempt is not None:
            mix ^= int(attempt) * 0x85EBCA6B
        return random.Random(mix & 0xFFFFFFFF)
    if rng is not None:
        return rng
    if optimization_iter is not None or attempt is not None:
        mix = 0xA11CE
        if optimization_iter is not None:
            mix ^= int(optimization_iter) * 0x9E3779B1
        if attempt is not None:
            mix ^= int(attempt) * 0x85EBCA6B
        return random.Random(mix & 0xFFFFFFFF)
    return random.Random()


def read_locked_write_frequencies(
    art_dir: Path | None,
    *,
    optimization_iter: int | None = None,
    attempt: int | None = None,
) -> list[int] | None:
    if art_dir is None:
        return None
    path = _lock_path(art_dir)
    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return None
    freqs = _parse_freq_list(payload.get("write_frequencies"))
    if not freqs:
        return None
    if optimization_iter is not None:
        locked_iter = payload.get("optimization_iter")
        try:
            if locked_iter is not None and int(locked_iter) != int(optimization_iter):
                return None
        except (TypeError, ValueError):
            return None
    if attempt is not None:
        locked_attempt = payload.get("attempt")
        try:
            if locked_attempt is not None and int(locked_attempt) != int(attempt):
                return None
        except (TypeError, ValueError):
            return None
    return freqs


def write_locked_write_frequencies(
    art_dir: Path,
    freqs: list[int],
    *,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    source: str = "sampled",
) -> Path:
    art_dir.mkdir(parents=True, exist_ok=True)
    path = _lock_path(art_dir)
    payload = {
        "write_frequencies": [int(f) for f in freqs],
        "optimization_iter": optimization_iter,
        "attempt": attempt,
        "source": source,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return path


def _sample_freqs(
    workload: dict[str, Any],
    *,
    optimization_iter: int | None,
    attempt: int | None,
    rng: random.Random | None,
) -> list[int]:
    base = base_write_frequencies(workload)
    if not sample_write_mix_enabled(workload):
        return _merge_freqs(base, [])
    local_rng = _rng_for_sample(
        workload,
        optimization_iter=optimization_iter,
        attempt=attempt,
        rng=rng,
    )
    extra = _sample_write_mix_freqs(workload, already=base, rng=local_rng)
    if not extra and not any(f > 0 for f in base):
        cfg = _sample_cfg(workload)
        extra = _sample_write_mix_freqs(
            {
                **workload,
                "aux_bench_sweep_optimization": {
                    **_opt_sweep(workload),
                    "write_mix_sample": {**cfg, "count": max(1, int(cfg.get("count", 1)))},
                },
            },
            already=base,
            rng=local_rng,
        )
    return _merge_freqs(base, extra)


def resolve_optimization_write_frequencies(
    workload: dict[str, Any],
    *,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    rng: random.Random | None = None,
    persist: bool = True,
) -> list[int]:
    """Frequencies for aux optimization quick bench.

    Always includes 0%. With ``write_mix_sample.enabled``, adds random write mixes
    locked per attempt under ``art_dir`` when available.
    """
    opt_iter = optimization_iter_from_env(optimization_iter)
    opt_attempt = attempt_from_env(attempt)

    if not sample_write_mix_enabled(workload):
        return _merge_freqs(base_write_frequencies(workload), [])

    if lock_per_attempt_enabled(workload) and art_dir is not None:
        locked = read_locked_write_frequencies(
            art_dir, optimization_iter=opt_iter, attempt=opt_attempt
        )
        if locked is not None:
            return locked
        # Mid-attempt tools may only know the current lock file.
        if opt_attempt is None:
            locked_any = read_locked_write_frequencies(art_dir, optimization_iter=opt_iter)
            if locked_any is not None:
                return locked_any

    freqs = _sample_freqs(
        workload,
        optimization_iter=opt_iter,
        attempt=opt_attempt,
        rng=rng,
    )
    if persist and lock_per_attempt_enabled(workload) and art_dir is not None:
        write_locked_write_frequencies(
            art_dir,
            freqs,
            optimization_iter=opt_iter,
            attempt=opt_attempt,
            source="write_mix_sample",
        )
    return freqs


def ensure_attempt_write_frequencies(
    art_dir: Path,
    workload: dict[str, Any],
    *,
    optimization_iter: int,
    attempt: int,
    rng: random.Random | None = None,
) -> list[int]:
    """Force (re)sample+lock for a specific optimization attempt (0 = iter baseline)."""
    os.environ["ACCEL_PIPELINE_OPT_ITER"] = str(optimization_iter)
    os.environ["ACCEL_PIPELINE_OPT_ATTEMPT"] = str(attempt)

    if not sample_write_mix_enabled(workload):
        freqs = _merge_freqs(base_write_frequencies(workload), [])
        write_locked_write_frequencies(
            art_dir,
            freqs,
            optimization_iter=optimization_iter,
            attempt=attempt,
            source="static",
        )
        return freqs

    # Always fresh for this attempt — drop any prior lock first.
    path = _lock_path(art_dir)
    if path.is_file():
        path.unlink(missing_ok=True)

    freqs = _sample_freqs(
        workload,
        optimization_iter=optimization_iter,
        attempt=attempt,
        rng=rng,
    )
    write_locked_write_frequencies(
        art_dir,
        freqs,
        optimization_iter=optimization_iter,
        attempt=attempt,
        source="write_mix_sample",
    )
    return freqs


# Back-compat alias used by older call sites / tests.
def optimization_iter_from_env(default: int | None = None) -> int | None:
    raw = os.environ.get("ACCEL_PIPELINE_OPT_ITER")
    if raw is None or not str(raw).strip():
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def attempt_from_env(default: int | None = None) -> int | None:
    raw = os.environ.get("ACCEL_PIPELINE_OPT_ATTEMPT")
    if raw is None or not str(raw).strip():
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def geometric_mean(values: list[float]) -> float:
    positives = [float(v) for v in values if v is not None and float(v) > 0]
    if not positives:
        return 0.0
    log_sum = sum(math.log(v) for v in positives)
    return math.exp(log_sum / len(positives))


def write_mix_speedups_from_by_freq(by_freq: dict[str, Any] | None) -> list[tuple[int, float]]:
    """Non-zero write-frequency workload speedups as (pct, speedup)."""
    if not isinstance(by_freq, dict):
        return []
    out: list[tuple[int, float]] = []
    for key, row in by_freq.items():
        if not isinstance(row, dict):
            continue
        try:
            pct = int(float(row.get("write_frequency_pct", key)))
        except (TypeError, ValueError):
            continue
        if pct <= 0:
            continue
        try:
            speedup = float(row.get("workload_speedup") or 0.0)
        except (TypeError, ValueError):
            continue
        if speedup > 0:
            out.append((pct, speedup))
    out.sort(key=lambda item: item[0])
    return out


def aggregate_write_mix_workload_speedup(
    metrics: dict[str, Any],
    *,
    prefer_direct: bool = True,
) -> tuple[float, list[int]]:
    """Write-mix axis for dual-axis score: geo-mean of all non-zero write points.

    Returns ``(speedup, write_mix_pcts)``.
    """
    if prefer_direct:
        direct = metrics.get("write_mix_workload_speedup")
        by_freq = metrics.get("by_write_frequency_pct") or {}
        if direct is not None and not by_freq:
            try:
                value = float(direct)
                if value > 0:
                    pcts_raw = metrics.get("write_mix_pcts")
                    if isinstance(pcts_raw, list) and pcts_raw:
                        pcts = [int(x) for x in pcts_raw]
                    else:
                        pct = metrics.get("write_mix_pct")
                        pcts = [int(pct)] if pct else []
                    return value, pcts
            except (TypeError, ValueError):
                pass

    pairs = write_mix_speedups_from_by_freq(metrics.get("by_write_frequency_pct"))
    if not pairs:
        direct = metrics.get("write_mix_workload_speedup")
        if direct is not None:
            try:
                value = float(direct)
                if value > 0:
                    pct = metrics.get("write_mix_pct")
                    return value, [int(pct)] if pct else []
            except (TypeError, ValueError):
                pass
        by_freq = metrics.get("by_write_frequency_pct") or {}
        for key in ("10", 10, "1", 1):
            row = by_freq.get(key) if isinstance(by_freq, dict) else None
            if not isinstance(row, dict):
                continue
            try:
                value = float(row.get("workload_speedup") or 0.0)
            except (TypeError, ValueError):
                continue
            if value > 0:
                return value, [int(key)]
        return 0.0, []

    pcts = [p for p, _ in pairs]
    return geometric_mean([s for _, s in pairs]), pcts
