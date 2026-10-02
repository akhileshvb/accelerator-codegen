# Verus pipeline: audit summary ({{benchmark}} q{{query}})

Read `analysis/audit_report.json` (written by the pipeline: trusted base,
frozen files, leak check, both stores' test results, the fault self-check,
benchmarks, the optimization history), `analysis/analysis.json` and the
kernel in `crate/src`.

Write `analysis/audit_summary.md` for an engineer deciding whether to trust
and use this accelerator: the verdict and why; what is proved and what is
trusted; correctness on both stores; performance (read speedup, write mixes,
Verus vs C++ store); the design and how it should scale (from the complexity
notes and the measurements); risks and anything that looked wrong. Be
factual; quote numbers from the report.

{{include:shared/rules.md}}
