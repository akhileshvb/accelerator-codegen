# Verification (Postgres aux kernels)

How the stages fit together (**FormalSQL → AuxAnswerSpec → FoldTwin → runtime**;
C++ = RepPhys + finite extract-diff): **[`PIPELINE.md`](PIPELINE.md)**.
Logos+FormalSQL pin: [`formalsql/PIN.md`](formalsql/PIN.md).

This README is the operational map (files, runners, guarantee tables).

**Authoritative SQL meaning is FormalSQL** (via Logos lower of exact catalog
SQL). Hand-written `Q*SqlSem` / `MathSem` are **deprecated as SQL authority**
(see PIPELINE §6); algebraic helpers may remain until moved into Aux/twin proofs.

Top-level runners:

```bash
scripts/run_verification_q1.sh
scripts/run_verification_q21.sh
scripts/run_verification_all.sh
```

Two layers for Q1, plus the same stack for Q21:

1. **Q1 structural inductive proof (Dafny)** — 1:1 twins of postgres
   `finalize.cpp` / `module.cpp`.
2. **Dafny extract ≡ generated C++** on a bounded grid D (`dafny build --target:cs`
   then dump-and-diff against the artifact).
3. **Q21** — same chain for the EXISTS/NOT EXISTS accelerator (see below).

Differential testing ([`tests/aux_state/test_postgres_tpch_differential.cpp`](../aux_state/test_postgres_tpch_differential.cpp))
remains the oracle that pins aux behavior to engine SQL.

---

## Q1 structural inductive proof (artifact-tied)

### Decisions

- Structural Dafny twin per accelerator-local function (same branches / erases /
  field updates as the C++), not a loose intent model.
- Avg columns included: twin of `HugeintScaleToDouble` (`real` in Dafny; IEEE
  doubles compared on grid D by converting the extracted rational).
- Substrate store/coordinator/maintenance early paths and Postgres backend
  routing are twin'd under [`scripts/run_dafny_backend_induction.sh`](../../scripts/run_dafny_backend_induction.sh);
  SPI/xact/SQL remain TCB / differentials.

### Guarantee chain (Q1 accelerator)

| Layer | What | Guarantee |
|-------|------|-----------|
| [`q1/dafny/Q1Finalize.dfy`](q1/dafny/Q1Finalize.dfy) | finalize.cpp twins + avgs | Unbounded algebra + avg closed form |
| [`q1/dafny/Q1ModuleTwin.dfy`](q1/dafny/Q1ModuleTwin.dfy) | module.cpp orchestration | Unbounded lemmas over oracles |
| [`q1/dafny/Q1Aux.dfy`](q1/dafny/Q1Aux.dfy) | Fold/Inv/Compose | Homomorphism / Inv |
| [`q1/dafny/Q1Maintenance.dfy`](q1/dafny/Q1Maintenance.dfy) | StageMaintenance-class actions | **Inv under maintenance** |
| [`q1/dafny/Q1MaintenanceEdges.dfy`](q1/dafny/Q1MaintenanceEdges.dfy) | Delete-any, Install/CAS, policy map | **Maintenance edges** |
| [`q1/dafny/generated/Q1AuxSpec.dfy`](q1/dafny/generated/Q1AuxSpec.dfy) | Shared AuxSpecOps / FoldSpec | Spec side of Twin≡Spec |
| [`q1/dafny/Q1TwinSpecBridge.dfy`](q1/dafny/Q1TwinSpecBridge.dfy) | TwinOp ≡ SpecOp | FoldTwin = FoldSpec bridge |
| [`q1/dafny/Q1Sql.dfy`](q1/dafny/Q1Sql.dfy) | **DEPRECATED as SQL authority** — algebraic helpers only | Use FormalSQL + AuxSpec |
| [`q1/dafny/Q1SqlSem.dfy`](q1/dafny/Q1SqlSem.dfy) | **DEPRECATED as SQL authority** — do not deepen | Logos → FormalSQL |
| [`formalsql/q1/`](formalsql/q1/) | Q1 FormalSQL Box 1 — see [`STATUS.md`](formalsql/q1/STATUS.md) | Obligations + spine facade |
| [`formalsql/lib/QuerySpineFacade.v`](formalsql/lib/QuerySpineFacade.v) | Generic TFGOP EQuery composition | Reusable FormalSQL spine |
| [`substrate/dafny/`](substrate/dafny/) | PairInv / SnapshotPinning / ReadHandle | Runtime substrate |
| [`q1/extract/`](q1/extract/) | Dafny C# extract ‖ generated C++ | **Agree on grid D** (not ∀) |
| [`q1/conformance/test_q1_sql_sem_conformance.cpp`](q1/conformance/test_q1_sql_sem_conformance.cpp) | MathSem filter/fold vs Finalize | SQL MathSem check on samples |

### Inv under maintenance

Machine-checked (`Q1Maintenance.MaintenancePreservesInv`): for Q1 aux state
with `Inv(store, table)`, each StageMaintenance-class action preserves Inv:

- **NoOp** — state unchanged
- **ApplyDelta** insert / insert-seq / delete-last / **delete-at (any index)** /
  delete-suffix / same-day update / **update-at**
- **RebuildPartition / RebuildAll** — `store := Fold(table)` establishes Inv
- **InvalidateAndFallback** — clears `valid`; store may still equal `Fold(table)`;
  Execute must use rebuild/fallback (`ExecuteFallsBackWhenInvalid`)

Structural twin case-split: `Q1ModuleTwin.StageMaintenanceCaseSplit`.

### Maintenance edges (`Q1MaintenanceEdges`)

- **Non-suffix delete:** `InvPreservedByDeleteAt` via per-cell `Contrib` (Fold
  order-independent).
- **Install/CAS:** successful `Publish`/`TryPublish` keeps readable Inv payload;
  CAS fail / `InvalidatePartition` ⇒ not readable (fallback path).
- **ChooseMaintenance:** early select / `MinSupported` only return safe actions;
  `ChosenPolicyPreservesInv` maps them to Inv-preserving `MaintAction`.

**Not claimed:** cost optimality of ChooseMaintenance; multi-partition Install
atomicity beyond per-key Publish/CAS; Postgres executor ≡ formal SQL Interpret.

### SQL ≡ MathSem ≡ aux (formal)

Machine-checked (Dafny), starting from catalog raw SQL:

```text
Sem(Parse(TpchQ1BaseSql, cutoff), T) = MathSem(T, cutoff)
                                     = Finalize(... AuxCompose(T, cutoff) ...)
```

- **`Q1SqlSem`:** `TpchQ1BaseSql` is the catalog `base_sql` text.
  `ParseTpchQ1BaseSql` builds a clause AST whose SELECT items are the expression
  trees from that SQL (`sum(l_extendedprice*(1-l_discount))`, etc.).
  `Sem` evaluates WHERE → GROUP BY → those SELECT exprs → ORDER BY (scale-2).
  Top lemma: `RawSqlSemEqualsMathSem`.
- **`Q1Sql`:** MathSem ≡ aux Compose/Finalize.

**TCB:** Postgres on `base_sql` ≡ `Sem(Parse(base_sql), ·)` (assumed).
Live engine ↔ aux still via differentials. Cutoff in the string is abstracted
(`DATE…-INTERVAL…` → `ShipdateLe(cutoff)`).

### Avg note

Dafny proves `HugeintScaleToDouble` ≡ `value / 10^scale / count` over `real`
(Dafny-only closed form; not IEEE).

Extract-diff dumps avgs as a rational \(n/d\) from the twin and as IEEE `double`
from generated Finalize. The comparator converts the rational to `double`
(`math.isclose`); this is **not** bit-identity.

### Dafny extract ≡ generated C++ (grid D)

Executable evidence, not a ∀ proof. `dafny build --target:cs` compiles
[`Q1ExtractMain.dfy`](q1/extract/Q1ExtractMain.dfy) (the twins themselves — no
handwritten `ModelApply`). [`q1_cpp_extract_diff.cpp`](q1/extract/q1_cpp_extract_diff.cpp)
runs the same literals on generated `finalize.cpp` / `module.cpp`. Integer
columns must match exactly.

**Grid D:** qty/price/disc/tax in the TPC-H-ish ranges used by Q1 conformance,
flags `{A,N,R}×{O,F}`, `ScaleToRaw` scales `{0,2,4}`, shipdates around 1995,
cutoff offsets `{0,60,90,120}`. Values stay inside `int64` / `Q1AccT`.

Compared:

- **Finalize:** Pack/Unpack, Apply, Merge, Finalize (integer columns exact).
- **Module, direct:** `TpchQ1CutoffDays`, `ScaleToRaw` (via `AccumulateMutation`
  on well-typed rows), txn buffer, `AccumulateMutation` control flow, encode then
  decode (byte header is C++-only; group maps compared).
- **Module via stubs:** `ApplyLineitemInput` + `ComposeOutput` by
  `ExecuteFallback` + scan stub; `ApplyDelta` erase-on-zero by
  `StageMaintenance` with `ChooseMaintenance` stubbed to `ApplyDelta` and
  [`InMemoryStore`](common/in_memory_store.hpp).

Not compared (TCB / not twinned): cell parsers, `WriteQ1Acc` header,
`CollectQ1DayPartitions`, `shared_ptr` images, `CheckValidity` implementation,
sink I/O beyond integer columns + avg rational↔double. Substrate
(`StateStore`, `ChooseMaintenance`, SPI) is stubbed.

```bash
./scripts/run_dafny_q1_extract_diff.sh   # requires dafny + dotnet + DuckDB sibling
```

### Install Dafny / run Q1

```bash
mkdir -p tools && unzip dafny-*-x64-ubuntu-*.zip -d tools/
./scripts/run_dafny_q1_induction.sh
```

**Claim after a green Q1 run:** accelerator twins + **formal SQL≡MathSem≡aux**
(`Q1SqlSem` + `Q1Sql`) + **Inv under maintenance** (incl. delete-any, Install/CAS,
policy→Inv map); Dafny extract ≡ generated kernels on grid D; SQL MathSem sample
check remains.

**Not claimed:** Postgres executor ≡ formal `Interpret` (TCB); `__int128` overflow
outside domains; pipeline/LLM codegen; ChooseMaintenance cost optimality;
unbounded ∀ agreement; Dafny `real` ≡ IEEE bit-identity.

---

## Q21 structural inductive proof (artifact-tied)

Same methodology as Q1 on postgres
[`tpch/q21/.../module.cpp`](../../src/generated/tpch/q21/postgres/aux/module.cpp) /
[`finalize.cpp`](../../src/generated/tpch/q21/postgres/aux/finalize.cpp). Hard
case: correlated `EXISTS` / `NOT EXISTS` reduced by `ComputeOrderContributions`
(sole-late supplier).

| Layer | What | Guarantee |
|-------|------|-----------|
| [`q21/dafny/Q21Finalize.dfy`](q21/dafny/Q21Finalize.dfy) | finalize sort/emit | BetterOutputRow + FinalizeFromEntries |
| [`q21/dafny/Q21ModuleTwin.dfy`](q21/dafny/Q21ModuleTwin.dfy) | module orchestration | Contrib reduction, txn, StageMaintenance, Execute |
| [`q21/dafny/Q21Aux.dfy`](q21/dafny/Q21Aux.dfy) | Fold/Inv/Compose | Multi-table Fold; repair via RecomputeOrder |
| [`q21/dafny/Q21Maintenance.dfy`](q21/dafny/Q21Maintenance.dfy) | StageMaintenance-class | **Inv under rebuild/invalidate** |
| [`q21/dafny/Q21MaintenanceEdges.dfy`](q21/dafny/Q21MaintenanceEdges.dfy) | Install/CAS, policy | **Maintenance edges** |
| [`q21/dafny/Q21Sql.dfy`](q21/dafny/Q21Sql.dfy) | Relational MathSem + sole-late bridge | **MathSem ≡ Finalize(entries)** |
| [`q21/dafny/Q21SqlSem.dfy`](q21/dafny/Q21SqlSem.dfy) | Parse catalog `base_sql` (EXISTS AST) | **`Sem(Parse)=MathSem`** |
| [`q21/extract/`](q21/extract/) | Dafny C# extract ‖ generated C++ | **Agree on grid D21** (not ∀) |
| [`postgres/dafny/Q21BackendProofs.dfy`](postgres/dafny/Q21BackendProofs.dfy) | SPI TCB composition | Execute≡Fallback; MathSem bridge; Install |

```bash
./scripts/run_dafny_q21_induction.sh
./scripts/run_dafny_q21_extract_diff.sh
```

**Claim after a green Q21 run:** twins + `Sem(Parse(base_sql))=MathSem` + Inv under
maintenance (rebuild/invalidate + Install/CAS) + extract ≡ C++ on D21; SPI/store/
`ChooseMaintenance`/parsers = TCB. Sole-late bag induction beyond structural
branches is discharged on D21 by construction; full ∀ lineitem-bag equivalence
of `SqlContribCount` ↔ `late_count` remains the documented bridge obligation.

**Not claimed:** Postgres executor ≡ Interpret; DuckDB/MySQL aux clones; nation-name
hash slots; ∀ extract agreement.

---

## Substrate + Postgres backend inductive POC

Same methodology (structural Dafny twin + C++ binding checks). **TCB:** PostgreSQL
SPI/xact/heap and shared substrate Store/Coordinator/policy are assumed correct.
Proved: generated Runtime + PostgresAuxBackend routing composed with Q1 module
twins (cold Build/Execute, mutation commit, MathSem bridge on SPI tables).

### Run

```bash
./scripts/run_dafny_backend_induction.sh
```

### Guarantee chain

| Layer | Artifact | Claim |
|-------|----------|-------|
| Tier A | [`substrate/dafny/Store.dfy`](substrate/dafny/Store.dfy) | Publish/TryPublish CAS fail/Invalidate readable contracts |
| Tier A | [`MaintenancePolicy.dfy`](substrate/dafny/MaintenancePolicy.dfy) | Forced override, zero-row NoOp, min-cost among supported |
| Tier A | [`Coordinator.dfy`](substrate/dafny/Coordinator.dfy) | Stage→Commit ApplyDelta publish readable (milestone lemma) |
| Tier B | [`postgres/dafny/PostgresBackend.dfy`](postgres/dafny/PostgresBackend.dfy) | CaptureMutation/PreCommit/Commit/Abort routing; no sync rebuild |
| Tier C | [`postgres/dafny/RuntimeScan.dfy`](postgres/dafny/RuntimeScan.dfy) | Q1 lineitem projection scales=2; EnsureBuilt/Execute glue |
| Tier D | [`postgres/dafny/BackendCompose.dfy`](postgres/dafny/BackendCompose.dfy) | Compose store Inv with Q1ModuleTwin Execute≡Fallback |
| **Q1 backend** | [`postgres/dafny/Q1BackendProofs.dfy`](postgres/dafny/Q1BackendProofs.dfy) | **SPI→Build→Execute ≡ Compose**; mutation commit; MathSem on SPI table; EnsureBuilt |
| **Q21 backend** | [`postgres/dafny/Q21BackendProofs.dfy`](postgres/dafny/Q21BackendProofs.dfy) | **Execute≡Fallback**; MathSem bridge; Install after maint |
| Conformance | `substrate/conformance`, `postgres/conformance` | Real PartitionedStateStore, SelectAuxMaintenanceAction, PostgresAuxBackend, projection twin |

### Generated-backend claim (`Q1BackendProofs`)

Under SPI+substrate TCB:

- `BuildFromScan(spi_rows)` → `Execute` with Inv ≡ `ComposeOutput` / Fallback
- `EnsureBuilt` / `ExecuteGlue` install readable payloads; rebuild reinstalls
- Capture→PreCommit→Commit publishes ApplyDelta; Abort clears mutations
- `SpiTableMathSemEqualsAux`: MathSem of SPI-derived table ≡ AuxCompose finalize
- Empty scan ⇒ empty result (= MathSem [])
- Maint Install after Inv-preserving action keeps readable payload

**Still not claimed:** PostgreSQL SPI correctness; substrate internals (TCB here);
full DayStore≡MathSem bit-equality for arbitrary tables (ComposeOutput vs
SqlFold path — empty + MathSem bridge proved; inductive DayStore merge open);
formal SQL beyond the Q1 fragment AST in `Q1SqlSem`.
### Anti-looseness

SOURCE maps in each `.dfy` header. Do not edit twins without updating C++ or
the extract-diff grid.

---

## Layout

```
tests/verification/
  common/          # InMemoryStore / CollectingSink for extract-diff
  q1/dafny/        # accelerator twins (Finalize / ModuleTwin / Aux)
  q1/extract/      # Dafny C# Main + C++ dump + avg comparator
  q1/conformance/  # SQL MathSem sample check
  q21/dafny/       # Q21 twins + MathSem/EXISTS + maintenance
  q21/extract/     # Dafny C# Main + C++ dump (grid D21)
  q21/conformance/ # sole-late / finalize sample check
  substrate/       # Store, MaintenancePolicy, Coordinator
  postgres/        # Backend adapters, RuntimeScan, BackendCompose, Q1/Q21 proofs
  README.md
scripts/run_dafny_q1_induction.sh
scripts/run_dafny_q1_extract_diff.sh
scripts/run_dafny_q21_induction.sh
scripts/run_dafny_q21_extract_diff.sh
scripts/run_dafny_backend_induction.sh
```
