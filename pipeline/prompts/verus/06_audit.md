# Verus pipeline: audit summary ({{benchmark}} q{{query}})

The audit report (written by the pipeline: trusted base, frozen files, leak
check, both stores' test results, the fault self-check, benchmarks, the
optimization history) and the analysis are inlined below; the kernel is in
`crate/src` (read what you need of it).

Write `analysis/audit_summary.md` for an engineer deciding whether to trust
and use this accelerator: the verdict and why; what is proved and what is
trusted; correctness on both stores; performance (read speedup, write mixes,
Verus global store vs C++ store); the design and how it should scale (from the complexity
notes and the measurements); risks and anything that looked wrong. Be
factual; quote numbers from the report.

## Material

You have these files in full below: do not re-read them. Work from this text and answer in a few steps; use tools only to check something against data.

{{file:analysis/audit_report.json}}

{{file:analysis/analysis.json}}

{{include:shared/rules.md}}
