# Verification (Postgres aux kernels)

Two layers:

1. **Bounded / concrete harnesses** (CBMC-oriented) for module-level props on real
   `TpchQNModule` code.
2. **Q1 structural inductive proof (Dafny)** — 1:1 twins of postgres
   `finalize.cpp` / `module.cpp`, bound by per-symbol C++ conformance.

Differential testing ([`tests/aux_state/test_postgres_tpch_differential.cpp`](../aux_state/test_postgres_tpch_differential.cpp))
remains the oracle that pins aux behavior to engine SQL.

---

## Q1 structural inductive proof (artifact-tied)

### Decisions

- Structural Dafny twin per accelerator-local function (same branches / erases /
  field updates as the C++), not a loose intent model.
- Avg columns included: twin of `HugeintScaleToDouble` (`real` in Dafny; IEEE
  doubles checked by conformance bit-patterns).
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
| [`q1/dafny/Q1Sql.dfy`](q1/dafny/Q1Sql.dfy) | MathSem of catalog `base_sql` (scale-2 embedding) | **SQL ≡ aux:** `MathSem = Finalize(Compose(...))` |
| [`q1/conformance/`](q1/conformance/) | Real finalize + module + MathSem twin | Twin ≡ artifact on grid |

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
atomicity beyond per-key Publish/CAS; Postgres executor ≡ MathSem.

### SQL ≡ aux (formal MathSem)

Machine-checked (Dafny):

```text
MathSem_Q1(T, cutoff) = FinalizeFromEntries(MapToEntries(AuxCompose(T, cutoff)))
```

where MathSem is the denotation of catalog TPC-H Q1 (`query_catalog.cpp` `base_sql`)
in the **scale-2 integer embedding** (`price*(100-disc)`, etc.), not IEEE SQL floats.
Bridge: `SqlAgg` (SQL sum formulas) ≡ `AggByApply` (`ApplyQ1RowContribution`).

**TCB:** Postgres SPI/executor implements MathSem — not proved; live `base_sql` ↔ aux
still via differentials.

### Avg note

Dafny proves `HugeintScaleToDouble` ≡ `value / 10^scale / count` over `real`.
Conformance requires exact `double` bit equality on the TPC-H-ish grid.

### Install Dafny / run Q1

```bash
mkdir -p tools && unzip dafny-*-x64-ubuntu-*.zip -d tools/
./scripts/run_dafny_q1_induction.sh
```

**Claim after a green Q1 run:** accelerator twins + **formal Q1 SQL≡aux** (MathSem) +
**Inv under maintenance** (incl. delete-any, Install/CAS, policy→Inv map);
conformance binds kernels and MathSem to artifacts.

**Not claimed:** Postgres executor ≡ MathSem; `__int128` overflow outside domains;
pipeline/LLM codegen; ChooseMaintenance cost optimality.

---

## Substrate + Postgres backend inductive POC

Same methodology (structural Dafny twin + C++ conformance). **TCB:** PostgreSQL
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
SQL ≡ MathSem beyond catalog embedding.
### Anti-looseness

SOURCE maps in each `.dfy` header. Do not edit twins without updating C++ or conformance.

---

## Bounded symbolic / concrete harnesses

CBMC-oriented harnesses for **Postgres** TPC-H Q1 and Q6 aux modules. They
drive the **real** generated `TpchQNModule` code with a headless in-memory store
and typed scan stubs — not the Postgres server or SQL executor.

### Properties

| Harness | Property |
|---------|----------|
| `q1/harness_kernel_homomorphism` | Apply/Merge order-independent; cancel |
| `q1/harness_encode_decode` | Encode ∘ Decode = id |
| `q1/harness_execute_eq_fallback` | Build; Execute ≡ ExecuteFallback |
| `q1/harness_applydelta_eq_rebuild` | ApplyDelta ≡ rebuild |
| `q6/harness_*` | Analogous for Q6 |

### How to run harnesses

```bash
./scripts/run_cbmc_postgres_aux.sh all all concrete
```

## Layout

```
tests/verification/
  q1/           # accelerator twins (Finalize / ModuleTwin / Aux)
  substrate/    # Store, MaintenancePolicy, Coordinator
  postgres/     # Backend adapters, RuntimeScan, BackendCompose
  README.md
scripts/run_dafny_q1_induction.sh
scripts/run_dafny_backend_induction.sh
```
