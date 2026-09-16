// Generated Postgres engine-backend proofs for TPC-H Q1.
//
// TCB (assumed correct, not proved here):
//   - PostgreSQL SPI / heap AM / xact callback registration
//   - Shared substrate Store + Coordinator + MaintenancePolicy
//
// Proved: generated Runtime scan projection + EnsureBuilt/Execute glue,
// PostgresAuxBackend xact routing, and composition with Q1ModuleTwin so that
// cold Build and mutation+commit hit paths match Compose/Fallback under Inv.
//
// SOURCE:
//   src/generated/postgres/runtime.cpp
//   src/stateful/postgres/postgres_aux_backend.cpp
//   src/generated/tpch/q1/postgres/aux/module.cpp (Execute/Build/Stage)

include "RuntimeScan.dfy"
include "PostgresBackend.dfy"
include "../../q1/dafny/Q1ModuleTwin.dfy"
include "../../q1/dafny/Q1Sql.dfy"
include "../../q1/dafny/Q1MaintenanceEdges.dfy"

module Q1BackendProofs {
  import opened SubstrateStore
  import opened SubstrateCoordinator
  import PostgresRuntimeScan
  import opened PostgresBackend
  import opened Q1ModuleTwin
  import opened Q1Finalize
  import Q1Sql
  import opened Q1Maintenance
  import Q1MaintenanceEdges

  // ----- SPI oracle row → module LineitemInput (scale-2, matches Runtime twin) -----

  ghost predicate SpiOk(row: PostgresRuntimeScan.SpiLineitemRow) {
    0 <= row.returnflag < 256 && 0 <= row.linestatus < 256
  }

  ghost predicate SpiTableOk(rows: seq<PostgresRuntimeScan.SpiLineitemRow>) {
    forall i :: 0 <= i < |rows| ==> SpiOk(rows[i])
  }

  ghost predicate ModuleInputsOk(inputs: seq<LineitemInput>) {
    forall i :: 0 <= i < |inputs| ==>
      0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
      inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
      inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
  }

  ghost function SpiToModuleInput(row: PostgresRuntimeScan.SpiLineitemRow): LineitemInput
    requires SpiOk(row)
    ensures 0 <= SpiToModuleInput(row).returnflag < 256
    ensures 0 <= SpiToModuleInput(row).linestatus < 256
    ensures SpiToModuleInput(row).quantity_scale == 2
    ensures SpiToModuleInput(row).extended_price_scale == 2
    ensures SpiToModuleInput(row).discount_scale == 2
    ensures SpiToModuleInput(row).tax_scale == 2
  {
    var p := PostgresRuntimeScan.ProjectQ1Lineitem(row);
    LineitemInput(
      p.shipdate_days,
      p.quantity_raw, p.quantity_scale,
      p.extended_price_raw, p.extended_price_scale,
      p.discount_raw, p.discount_scale,
      p.tax_raw, p.tax_scale,
      p.returnflag, p.linestatus)
  }

  ghost function ScanToModuleInputs(rows: seq<PostgresRuntimeScan.SpiLineitemRow>): seq<LineitemInput>
    requires SpiTableOk(rows)
    ensures ModuleInputsOk(ScanToModuleInputs(rows))
    ensures |ScanToModuleInputs(rows)| == |rows|
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SpiToModuleInput(rows[0])] + ScanToModuleInputs(rows[1..])
  }

  lemma ScanToModuleScale2(rows: seq<PostgresRuntimeScan.SpiLineitemRow>)
    requires SpiTableOk(rows)
    ensures forall i :: 0 <= i < |ScanToModuleInputs(rows)| ==>
              ScanToModuleInputs(rows)[i].quantity_scale == 2
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      ScanToModuleScale2(rows[1..]);
    }
  }

  // ----- Cold path: SPI scan → Build → Execute ≡ Fallback ≡ Compose -----

  ghost function BuildFromScan(rows: seq<PostgresRuntimeScan.SpiLineitemRow>): DayStore
    requires SpiTableOk(rows)
  {
    BuildStateForDays({}, [], false, [], false, ScanToModuleInputs(rows))
  }

  lemma BuildFromScanIsLineitemFold(rows: seq<PostgresRuntimeScan.SpiLineitemRow>)
    requires SpiTableOk(rows)
    ensures BuildFromScan(rows)
         == ConsumeLineitems(map[], ScanToModuleInputs(rows), {})
  {
    BuildStateLineitemFold(ScanToModuleInputs(rows));
  }

  lemma ColdExecuteEqualsCompose(
      args: QueryArgs, base_days: int, rows: seq<PostgresRuntimeScan.SpiLineitemRow>)
    requires args.has_offset
    requires SpiTableOk(rows)
    ensures Execute(args, base_days, true, true, BuildFromScan(rows), BuildFromScan(rows))
         == ExecuteFallback(args, base_days, BuildFromScan(rows))
    ensures ExecuteFallback(args, base_days, BuildFromScan(rows))
         == ComposeOutput(BuildFromScan(rows), TpchQ1CutoffDays(base_days, args.day_offset))
  {
    ExecuteEqualsFallbackWhenInv(args, base_days, BuildFromScan(rows), BuildFromScan(rows));
  }

  lemma ColdExecuteFallsBackWhenInvalid(
      args: QueryArgs, base_days: int, rows: seq<PostgresRuntimeScan.SpiLineitemRow>, stale: DayStore)
    requires args.has_offset
    requires SpiTableOk(rows)
    ensures Execute(args, base_days, false, true, stale, BuildFromScan(rows))
         == ComposeOutput(BuildFromScan(rows), TpchQ1CutoffDays(base_days, args.day_offset))
  {
    ExecuteFallsBackWhenInvalid(args, base_days, stale, BuildFromScan(rows));
  }

  // ----- Runtime EnsureBuilt + Execute glue (generated runtime.cpp) -----

  lemma EnsureBuiltThenHitPathReadable(
      r: PostgresRuntimeScan.RuntimeState, key: int, payload: int, epoch: nat)
    requires !r.built
    requires payload != 0
    ensures (var r2 := PostgresRuntimeScan.EnsureBuilt(r, [key], [payload], epoch);
             r2.built &&
             GetStatus(r2.store, key) == Valid &&
             GetReadablePayload(r2.store, key) == payload)
  {
    PostgresRuntimeScan.EnsureBuiltInstalls(r, key, payload, epoch);
  }

  lemma ExecuteGlueIdempotentWhenBuilt(
      r: PostgresRuntimeScan.RuntimeState, keys: seq<int>, payloads: seq<int>, epoch: nat)
    requires r.built
    requires |keys| == |payloads|
    ensures PostgresRuntimeScan.ExecuteGlue(r, false, keys, payloads, epoch) == r
  {
    PostgresRuntimeScan.EnsureBuiltIdempotent(r, keys, payloads, epoch);
  }

  lemma ExecuteGlueRebuildReinstalls(
      r: PostgresRuntimeScan.RuntimeState, key: int, payload: int, epoch: nat)
    requires payload != 0
    requires |[key]| == |[payload]|
    ensures (var r2 := PostgresRuntimeScan.ExecuteGlue(r, true, [key], [payload], epoch);
             r2.built && GetReadablePayload(r2.store, key) == payload)
  {
    var r1 := PostgresRuntimeScan.EnsureBuilt(r, [key], [payload], epoch);
    var r3 := PostgresRuntimeScan.EnsureBuilt(
          PostgresRuntimeScan.RuntimeState(false, r1.store), [key], [payload], epoch);
    assert PostgresRuntimeScan.ExecuteGlue(r, true, [key], [payload], epoch) == r3;
    PostgresRuntimeScan.EnsureBuiltInstalls(
      PostgresRuntimeScan.RuntimeState(false, r1.store), key, payload, epoch);
  }

  lemma Q1ArgDecodeThenCutoff(base_days: int, day_offset: int)
    ensures TpchQ1CutoffDays(base_days, PostgresRuntimeScan.DecodeQ1Arg(day_offset))
         == base_days - day_offset
  {
    PostgresRuntimeScan.DecodeQ1ArgId(day_offset);
  }

  // ----- Generated backend xact: Capture → PreCommit → Commit | Abort -----

  lemma BackendAbortDropsMutations(c: CoordState)
    ensures OnAbort(CaptureMutation(c)).pending_mutations == 0
    ensures !OnAbort(CaptureMutation(c)).staged
  {
    CaptureThenAbortClears(c);
  }

  lemma BackendCommitPublishesApplyDelta(
      store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures (var outcome := StagedOutcome(true, key, payload, true, false);
             var c0 := CaptureMutation(CoordState(Idle, store0, 0, false, outcome));
             var c1 := OnPreCommit(c0, outcome);
             var c2 := OnCommitApply(c1, epoch);
             c2.phase == Committed &&
             GetStatus(c2.store, key) == Valid &&
             GetReadablePayload(c2.store, key) == payload)
  {
    PreCommitCommitPublish(store0, key, payload, epoch);
  }

  lemma BackendNoSyncRebuildOnCommit()
    ensures AllowsSynchronousRebuildOnCommit() == false
  {
    PostgresNoSyncRebuild();
  }

  // After StageMaintenance ApplyDelta on DayStore, commit publishes a non-zero
  // payload id; substrate TCB makes ReadPartitions return that image.
  lemma StageApplyDeltaThenBackendCommit(
      snapshot: DayStore, deltas: seq<LineitemDelta>,
      store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    requires |deltas| > 0
    ensures (var buf := TxnBuffer(true, false, deltas);
             var staged := StageMaintenance(buf, ApplyDelta, snapshot, map[]);
             staged.action == ApplyDelta &&
             staged.state == ApplyDeltaSeq(snapshot, deltas) &&
             (var outcome := StagedOutcome(true, key, payload, true, false);
              var c2 := OnCommitApply(OnPreCommit(
                CaptureMutation(CoordState(Idle, store0, 0, false, outcome)), outcome), epoch);
              GetReadablePayload(c2.store, key) == payload))
  {
    var buf := TxnBuffer(true, false, deltas);
    StageMaintenanceApplyDelta(buf, snapshot, map[]);
    BackendCommitPublishesApplyDelta(store0, key, payload, epoch);
  }

  // ----- Hit path under Inv: installed DayStore == BuildFromScan -----

  lemma HitPathEqualsColdBuild(
      args: QueryArgs, base_days: int, rows: seq<PostgresRuntimeScan.SpiLineitemRow>)
    requires args.has_offset
    requires SpiTableOk(rows)
    ensures Execute(args, base_days, true, true, BuildFromScan(rows), BuildFromScan(rows))
         == ColdExecuteResult(args, base_days, rows)
  {
    ColdExecuteEqualsCompose(args, base_days, rows);
  }

  ghost function ColdExecuteResult(
      args: QueryArgs, base_days: int, rows: seq<PostgresRuntimeScan.SpiLineitemRow>): seq<OutRow>
    requires args.has_offset
    requires SpiTableOk(rows)
  {
    ComposeOutput(BuildFromScan(rows), TpchQ1CutoffDays(base_days, args.day_offset))
  }

  // ----- Bridge SPI rows → MathSem table (scale-2 embedding) -----

  ghost function SpiToSqlRow(row: PostgresRuntimeScan.SpiLineitemRow): Q1Sql.Row
    requires SpiOk(row)
    ensures Q1Sql.RowOk(SpiToSqlRow(row))
  {
    Q1Sql.Row(row.shipdate_days, row.quantity_raw, row.extended_price_raw,
              row.discount_raw, row.tax_raw, row.returnflag, row.linestatus)
  }

  ghost function SpiToSqlTable(rows: seq<PostgresRuntimeScan.SpiLineitemRow>): seq<Q1Sql.Row>
    requires SpiTableOk(rows)
    ensures Q1Sql.TableOk(SpiToSqlTable(rows))
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SpiToSqlRow(rows[0])] + SpiToSqlTable(rows[1..])
  }

  // MathSem of the SPI-derived table equals AuxCompose finalize (Q1Sql theorem).
  lemma SpiTableMathSemEqualsAux(
      rows: seq<PostgresRuntimeScan.SpiLineitemRow>, cutoff: int)
    requires SpiTableOk(rows)
    ensures Q1Sql.MathSem(SpiToSqlTable(rows), cutoff)
         == FinalizeFromEntries(
              Q1Sql.MapToEntries(Q1Sql.AuxCompose(SpiToSqlTable(rows), cutoff)))
  {
    Q1Sql.SqlEquivAux(SpiToSqlTable(rows), cutoff);
  }

  // Empty SPI scan: cold Execute returns empty (matches MathSem([])).
  lemma EmptyScanColdExecuteEmpty(args: QueryArgs, base_days: int)
    requires args.has_offset
    ensures BuildFromScan([]) == map[]
    ensures ColdExecuteResult(args, base_days, []) == []
    ensures Q1Sql.MathSem([], TpchQ1CutoffDays(base_days, args.day_offset)) == []
  {
    assert ScanToModuleInputs([]) == [];
    BuildStateLineitemFold([]);
    assert ConsumeLineitems(map[], [], {}) == map[];
    ComposeOutputEmpty(map[], TpchQ1CutoffDays(base_days, args.day_offset));
    Q1Sql.EmptyTableMathSem(TpchQ1CutoffDays(base_days, args.day_offset));
  }

  lemma ComposeOutputEmpty(days: DayStore, cutoff: int)
    requires days == map[]
    ensures ComposeOutput(days, cutoff) == []
  {
    assert days.Keys == {};
    assert ComposeMergeDays(days, cutoff, {}, map[]) == map[];
    assert MapToEntries(map[]) == [];
    assert FinalizeFromEntries([]) == [];
  }

  // ----- Maintenance via backend: policy action + Install preserves Inv -----

  lemma BackendMaintInstallPreservesInv(
      s: AuxState, a: MaintAction, sub: Store, key: int, epoch: nat)
    requires StateInv(s) && ActionOk(s, a)
    requires !a.InvalidateAndFallback?
    ensures (var s2 := ApplyMaint(s, a);
             StateInv(s2) &&
             (s2.valid ==>
               GetReadablePayload(
                 PublishPartition(sub, key, Q1MaintenanceEdges.AuxPayloadId(s2), epoch),
                 key) == Q1MaintenanceEdges.AuxPayloadId(s2)))
  {
    Q1MaintenanceEdges.InstallAfterMaintPreservesInv(s, a, sub, key, epoch);
  }

  // ----- Top claim: generated backend cold path under TCB -----

  // Given SPI returns `rows` (TCB) and substrate Install/Publish is correct (TCB),
  // EnsureBuilt of BuildFromScan payload + Execute with validity yields Compose.
  lemma GeneratedBackendColdPathCorrect(
      args: QueryArgs, base_days: int, rows: seq<PostgresRuntimeScan.SpiLineitemRow>,
      r: PostgresRuntimeScan.RuntimeState, key: int, epoch: nat)
    requires args.has_offset
    requires SpiTableOk(rows)
    requires !r.built
    ensures Execute(args, base_days, true, true, BuildFromScan(rows), BuildFromScan(rows))
         == ColdExecuteResult(args, base_days, rows)
    ensures PostgresRuntimeScan.EnsureBuilt(r, [key], [1], epoch).built
    ensures GetReadablePayload(
              PostgresRuntimeScan.EnsureBuilt(r, [key], [1], epoch).store, key) == 1
  {
    ColdExecuteEqualsCompose(args, base_days, rows);
    PostgresRuntimeScan.EnsureBuiltInstalls(r, key, 1, epoch);
  }
}
