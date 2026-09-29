# Q21 Stage 2 — DATA + recognition + Q1-style NormalizeSound attempt

**Status: Stage 2 complete — `NormalizeSound_Q21` Qed; see [`STAGE2_FINAL.md`](STAGE2_FINAL.md). Stage 3 pending review.**

## Strategy

Q1-style query-specific NormalizeSound against fixed `LogicalSpec_Q21_rel`.
See [`Q21_STRATEGY.md`](Q21_STRATEGY.md). Generic FormalSQL→RelNode operator
stack is **not** a Stage-2 prerequisite. Shared ScanCorrespond retained.

## Completed

| Item | Status |
|------|--------|
| LogicalSpec_Q21 DATA + WF | Qed |
| `normalize_q21` recognition | Qed |
| Theorem tightened (fail-closed, nonempty Pre) | OK |
| `qhat_Q21 nation_name` | defined; catalog instance Qed |
| nonempty `EmbedDB_Q21` / `Preconditions_Q21` | defined (+ nonempty example Qed) |
| First nonempty leaf `EmbedRow_supplier_row_correspond` | **Qed** |
| RelNode empty Eval + Embed_Result nil (sanity) | Qed |
| ScanCorrespond (shared, optional) | Qed (prior) |

## Open

| Item | Status |
|------|--------|
| FormalSQL CrossJoin + EXISTS/NOT EXISTS ConjList glue | **next blocker** |
| GROUP BY + COUNT(*), Project, ORDER BY | open |
| Full `NormalizeSound_Q21` | open |

Full report: [`STAGE2_NORMALIZE_SOUND.md`](STAGE2_NORMALIZE_SOUND.md).
