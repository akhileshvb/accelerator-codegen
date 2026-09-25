// Compiled structural twin of src/generated/tpch/q1/postgres/aux/module.cpp
// (ordinary functions, not ghost specs). Lemmas remain ghost.
//
// DayPart/DayStore keep a key-order seq as the compilable witness for C++
// unordered_map iteration (`:|` / set pick is not compilable). Finalize sorts
// by group_key, Encode sorts keys — same observable order as C++.
//
// SOURCE MAP (do not edit without updating C++ or the extract-diff grid):
//   ScaleToRaw                 module.cpp:~143
//   ApplyLineitemInput         module.cpp:~204
//   EncodeTpchQ1Day            module.cpp:~222  (payload entries; header opaque)
//   DecodeTpchQ1Day            module.cpp:~244
//   DaysFromCivil              module.cpp:~32
//   TpchQ1CutoffDays           module.cpp:~263
//   TpchQ1TxnBuffer::*         module.cpp:~267
//   InterpretMutation          module.cpp:~312
//   AccumulateMutation         module.cpp:~323
//   StageMaintenance ApplyDelta loop  module.cpp:~444
//   BuildStateForDays          module.cpp:~516
//   ComposeOutput              module.cpp:~553
//   LoadPartitions             module.cpp:~568
//   Execute / ExecuteFallback  module.cpp:~472 / ~498
//   EmitResult                 module.cpp:~592
//
// Substrate (StateStore, ChooseMaintenance, InitialScan, Sink) = oracles.

include "Q1Finalize.dfy"

module Q1ModuleTwin {
  import opened Q1Finalize

  // ----- ScaleToRaw (module.cpp:~143) -----

  function Pow10Nat(n: nat): int
    ensures Pow10Nat(n) > 0
  {
    if n == 0 then 1 else 10 * Pow10Nat(n - 1)
  }

  function ScaleToRaw(raw: int, scale: int, target_scale: int): int
    requires scale >= 0 && target_scale >= 0
  {
    if scale == target_scale then
      raw
    else if scale < target_scale then
      raw * Pow10Nat((target_scale - scale) as nat)
    else
      raw / Pow10Nat((scale - target_scale) as nat)
  }

  lemma ScaleToRawIdentity(raw: int)
    ensures ScaleToRaw(raw, 2, 2) == raw
  {
  }

  lemma ScaleToRawUp(raw: int, scale: nat, target: nat)
    requires scale < target
    ensures ScaleToRaw(raw, scale, target) == raw * Pow10Nat(target - scale)
  {
  }

  // ----- Day partition / store (groups + insertion-order keys) -----

  datatype DayPart = DayPart(groups: map<int, Stats>, keys: seq<int>)
  datatype DayStore = DayStore(parts: map<int, DayPart>, order: seq<int>)

  function EmptyPart(): DayPart {
    DayPart(map[], [])
  }

  function EmptyStore(): DayStore {
    DayStore(map[], [])
  }

  function GetGroup(part: DayPart, gkey: int): Stats {
    if gkey in part.groups then part.groups[gkey] else EmptyStats()
  }

  function PutGroup(part: DayPart, gkey: int, stats: Stats): DayPart {
    if gkey in part.groups then
      DayPart(part.groups[gkey := stats], part.keys)
    else
      DayPart(part.groups[gkey := stats], part.keys + [gkey])
  }

  function FilterKey(keys: seq<int>, gkey: int): seq<int>
    decreases |keys|
  {
    if |keys| == 0 then
      []
    else if keys[0] == gkey then
      FilterKey(keys[1..], gkey)
    else
      [keys[0]] + FilterKey(keys[1..], gkey)
  }

  function EraseGroup(part: DayPart, gkey: int): DayPart {
    DayPart(part.groups - {gkey}, FilterKey(part.keys, gkey))
  }

  function GetPart(state: DayStore, day: int): DayPart {
    if day in state.parts then state.parts[day] else EmptyPart()
  }

  function PutPart(state: DayStore, day: int, part: DayPart): DayStore {
    if day in state.parts then
      DayStore(state.parts[day := part], state.order)
    else
      DayStore(state.parts[day := part], state.order + [day])
  }

  // ----- ApplyLineitemInput (module.cpp:~204) -----

  datatype LineitemInput = LineitemInput(
    shipdate_days: int,
    quantity_raw: int,
    quantity_scale: int,
    extended_price_raw: int,
    extended_price_scale: int,
    discount_raw: int,
    discount_scale: int,
    tax_raw: int,
    tax_scale: int,
    returnflag: int,
    linestatus: int
  )

  function ApplyLineitemInput(input: LineitemInput, day: DayPart): DayPart
    requires 0 <= input.returnflag < 256 && 0 <= input.linestatus < 256
    requires input.quantity_scale >= 0 && input.extended_price_scale >= 0
    requires input.discount_scale >= 0 && input.tax_scale >= 0
  {
    var group_key := PackQ1GroupKey(input.returnflag, input.linestatus);
    var group := GetGroup(day, group_key);
    var group2 := ApplyQ1RowContribution(
      group,
      ScaleToRaw(input.quantity_raw, input.quantity_scale, 2),
      ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2),
      ScaleToRaw(input.discount_raw, input.discount_scale, 2),
      ScaleToRaw(input.tax_raw, input.tax_scale, 2),
      1);
    PutGroup(day, group_key, group2)
  }

  // ----- Typed delta (AccumulateMutation → StageMaintenance) -----

  datatype LineitemDelta = LineitemDelta(
    shipdate_days: int,
    group_key: int,
    quantity_raw: int,
    extended_price_raw: int,
    discount_raw: int,
    tax_raw: int,
    sign: int
  )

  // Apply one delta to DayStore with erase-on-zero (module.cpp:~444–450).
  function ApplyDeltaToStore(state: DayStore, d: LineitemDelta): DayStore {
    var day := d.shipdate_days;
    var part := GetPart(state, day);
    var group := GetGroup(part, d.group_key);
    var group2 := ApplyQ1RowContribution(
      group, d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw, d.sign);
    if group2.count == 0 then
      PutPart(state, day, EraseGroup(part, d.group_key))
    else
      PutPart(state, day, PutGroup(part, d.group_key, group2))
  }

  function ApplyDeltaSeq(state: DayStore, ds: seq<LineitemDelta>): DayStore
    decreases |ds|
  {
    if |ds| == 0 then state
    else ApplyDeltaSeq(ApplyDeltaToStore(state, ds[0]), ds[1..])
  }

  lemma ApplyDeltaSeqEmpty(state: DayStore)
    ensures ApplyDeltaSeq(state, []) == state
  {
  }

  lemma ApplyDeltaSeqStep(state: DayStore, d: LineitemDelta, ds: seq<LineitemDelta>)
    ensures ApplyDeltaSeq(state, [d] + ds)
         == ApplyDeltaSeq(ApplyDeltaToStore(state, d), ds)
  {
  }

  // Insert then matching delete: ApplyCancel on the cell (StageMaintenance erase path).
  lemma DeltaApplyCancelOnCell(g: Stats, d: LineitemDelta)
    requires d.sign == 1
    ensures ApplyQ1RowContribution(
              ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                     d.discount_raw, d.tax_raw, 1),
              d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw, -1) == g
  {
    ApplyCancel(g, d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw);
  }

  lemma ApplyDeltaToStoreInsertKeepsNonZero(state: DayStore, d: LineitemDelta)
    requires d.sign == 1
    requires var part := GetPart(state, d.shipdate_days);
             var g := GetGroup(part, d.group_key);
             g.count + 1 != 0
    ensures var state1 := ApplyDeltaToStore(state, d);
            d.shipdate_days in state1.parts &&
            d.group_key in state1.parts[d.shipdate_days].groups &&
            state1.parts[d.shipdate_days].groups[d.group_key].count != 0
  {
    var day := d.shipdate_days;
    var part := GetPart(state, day);
    var g := GetGroup(part, d.group_key);
    var g1 := ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                     d.discount_raw, d.tax_raw, 1);
    assert g1.count == g.count + 1;
    assert g1.count != 0;
  }

  // ----- DaysFromCivil / TpchQ1CutoffDays (module.cpp:~32 / ~263) -----
  // C++ kQ1BaseDays = DaysFromCivil(1998,12,1). Two-arg form keeps base symbolic
  // for lemmas; FromOffset is the exact generated kernel.

  function DaysFromCivil(year: int, month: int, day: int): int
    requires year >= 1
    requires 1 <= month <= 12
    requires 1 <= day <= 31
  {
    var y := year - (if month <= 2 then 1 else 0);
    var era := if y >= 0 then y / 400 else (y - 399) / 400;
    var yoe := y - era * 400;
    var doy := (153 * (month + (if month > 2 then -3 else 9)) + 2) / 5 + day - 1;
    var doe := yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
  }

  const kQ1BaseDays: int := DaysFromCivil(1998, 12, 1)
  const kTpchMinLineitemShipdateDays: int := DaysFromCivil(1992, 1, 2)

  function TpchQ1CutoffDays(base_days: int, day_offset: int): int {
    base_days - day_offset
  }

  function TpchQ1CutoffDaysFromOffset(day_offset: int): int {
    kQ1BaseDays - day_offset
  }

  lemma CutoffMonotone(base: int, o1: int, o2: int)
    requires o1 <= o2
    ensures TpchQ1CutoffDays(base, o2) <= TpchQ1CutoffDays(base, o1)
  {
  }

  lemma CutoffFromOffsetMatchesBase(day_offset: int)
    ensures TpchQ1CutoffDaysFromOffset(day_offset)
         == TpchQ1CutoffDays(kQ1BaseDays, day_offset)
  {
  }

  // ----- TxnBuffer (module.cpp:~267) -----

  datatype TxnBuffer = TxnBuffer(
    pending: bool,
    requires_rebuild: bool,
    deltas: seq<LineitemDelta>
  )

  function EmptyTxnBuffer(): TxnBuffer {
    TxnBuffer(false, false, [])
  }

  function TxnClear(buf: TxnBuffer): TxnBuffer {
    EmptyTxnBuffer()
  }

  function TxnMarkRebuild(b: TxnBuffer): TxnBuffer {
    TxnBuffer(true, true, [])
  }

  function TxnAddDelta(b: TxnBuffer, d: LineitemDelta): TxnBuffer {
    TxnBuffer(true, b.requires_rebuild, b.deltas + [d])
  }

  lemma TxnMarkRebuildClearsDeltas(b: TxnBuffer)
    ensures TxnMarkRebuild(b).deltas == []
    ensures TxnMarkRebuild(b).requires_rebuild
  {
  }

  // ----- InterpretMutation (module.cpp:~312) -----

  datatype InvalidationScope = ScopeNone | ScopePartitionSet | ScopeAll

  function InterpretMutation(lineitem_table: int, mutation_table: int): InvalidationScope {
    if mutation_table == lineitem_table then ScopePartitionSet else ScopeNone
  }

  lemma InterpretIrrelevant(lineitem_table: int, other: int)
    requires other != lineitem_table
    ensures InterpretMutation(lineitem_table, other) == ScopeNone
  {
  }

  // ----- AccumulateMutation control flow (module.cpp:~323) -----

  datatype MutationKind = Insert | Delete | Update

  datatype ParsedRow = ParsedRow(ok: bool, delta: LineitemDelta)

  // Oracle: ReadLineitem success/failure + produced delta.
  function AccumulateMutation(
    lineitem_table: int,
    mutation_table: int,
    kind: MutationKind,
    // parallel arrays of parse results for old/new rows
    old_parsed: seq<ParsedRow>,
    new_parsed: seq<ParsedRow>,
    buf: TxnBuffer
  ): TxnBuffer {
    if mutation_table != lineitem_table then
      buf
    else
      match kind
        case Insert =>
          AccumulateInserts(new_parsed, buf)
        case Delete =>
          AccumulateDeletes(old_parsed, buf)
        case Update =>
          if |old_parsed| != |new_parsed| then
            TxnMarkRebuild(buf)
          else
            AccumulateUpdates(old_parsed, new_parsed, buf)
  }

  function AccumulateInserts(rows: seq<ParsedRow>, buf: TxnBuffer): TxnBuffer
    decreases |rows|
  {
    if |rows| == 0 then
      buf
    else if !rows[0].ok then
      TxnMarkRebuild(buf)
    else
      AccumulateInserts(rows[1..], TxnAddDelta(buf, rows[0].delta))
  }

  function AccumulateDeletes(rows: seq<ParsedRow>, buf: TxnBuffer): TxnBuffer
    decreases |rows|
  {
    if |rows| == 0 then
      buf
    else if !rows[0].ok then
      TxnMarkRebuild(buf)
    else
      AccumulateDeletes(rows[1..], TxnAddDelta(buf, rows[0].delta))
  }

  function AccumulateUpdates(old_rows: seq<ParsedRow>, new_rows: seq<ParsedRow>, buf: TxnBuffer): TxnBuffer
    requires |old_rows| == |new_rows|
    decreases |old_rows|
  {
    if |old_rows| == 0 then
      buf
    else if !old_rows[0].ok || !new_rows[0].ok then
      TxnMarkRebuild(buf)
    else if old_rows[0].delta.shipdate_days != new_rows[0].delta.shipdate_days then
      TxnMarkRebuild(buf)
    else
      var b1 := TxnAddDelta(buf, old_rows[0].delta); // sign -1 already in delta
      var b2 := TxnAddDelta(b1, new_rows[0].delta);
      AccumulateUpdates(old_rows[1..], new_rows[1..], b2)
  }

  lemma AccumulateIrrelevantNoOp(buf: TxnBuffer, kind: MutationKind,
                                 old_p: seq<ParsedRow>, new_p: seq<ParsedRow>)
    ensures AccumulateMutation(1, 2, kind, old_p, new_p, buf) == buf
  {
  }

  // ----- StageMaintenance decision cases (module.cpp:~371) -----

  datatype MaintenanceAction = NoOp | ApplyDelta | RebuildPartition | RebuildAll | Other

  datatype StageOutcome = StageOutcome(
    action: MaintenanceAction,
    state: DayStore,           // resulting day map for ApplyDelta / RebuildPartition
    touched: set<int>
  )

  // Structural case split matching StageMaintenance.
  function StageMaintenance(
    buf: TxnBuffer,
    decision: MaintenanceAction,
    // oracle: current store snapshot for touched days
    snapshot: DayStore,
    // oracle: BuildStateForDays result when RebuildPartition
    rebuilt: DayStore
  ): StageOutcome {
    if buf.requires_rebuild then
      StageOutcome(
        if decision == ApplyDelta then RebuildAll else decision,
        snapshot,
        {})
    else if |buf.deltas| == 0 then
      StageOutcome(NoOp, snapshot, {})
    else
      var touched := TouchedDays(buf.deltas);
      if decision == RebuildPartition then
        StageOutcome(RebuildPartition, rebuilt, touched)
      else if decision != ApplyDelta then
        StageOutcome(decision, snapshot, touched)
      else
        StageOutcome(ApplyDelta, ApplyDeltaSeq(snapshot, buf.deltas), touched)
  }

  function TouchedDays(ds: seq<LineitemDelta>): set<int> {
    TouchedDaysFrom(ds, 0)
  }

  function TouchedDaysFrom(ds: seq<LineitemDelta>, i: nat): set<int>
    requires i <= |ds|
    decreases |ds| - i
  {
    if i == |ds| then
      {}
    else
      {ds[i].shipdate_days} + TouchedDaysFrom(ds, i + 1)
  }

  lemma StageMaintenanceApplyDelta(buf: TxnBuffer, snapshot: DayStore, rebuilt: DayStore)
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    ensures StageMaintenance(buf, ApplyDelta, snapshot, rebuilt).action == ApplyDelta
    ensures StageMaintenance(buf, ApplyDelta, snapshot, rebuilt).state
         == ApplyDeltaSeq(snapshot, buf.deltas)
  {
  }

  lemma StageMaintenanceRebuildFlag(buf: TxnBuffer, decision: MaintenanceAction,
                                    snapshot: DayStore, rebuilt: DayStore)
    requires buf.requires_rebuild
    ensures StageMaintenance(buf, decision, snapshot, rebuilt).action
         == (if decision == ApplyDelta then RebuildAll else decision)
  {
  }

  lemma StageMaintenanceNoOpEmptyDeltas(buf: TxnBuffer, decision: MaintenanceAction,
                                        snapshot: DayStore, rebuilt: DayStore)
    requires !buf.requires_rebuild
    requires |buf.deltas| == 0
    ensures StageMaintenance(buf, decision, snapshot, rebuilt)
         == StageOutcome(NoOp, snapshot, {})
  {
  }

  lemma StageMaintenanceRebuildPartitionUsesOracle(
      buf: TxnBuffer, snapshot: DayStore, rebuilt: DayStore)
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    ensures StageMaintenance(buf, RebuildPartition, snapshot, rebuilt).action
         == RebuildPartition
    ensures StageMaintenance(buf, RebuildPartition, snapshot, rebuilt).state
         == rebuilt
    ensures StageMaintenance(buf, RebuildPartition, snapshot, rebuilt).touched
         == TouchedDays(buf.deltas)
  {
  }

  lemma StageMaintenanceNonApplyInvalidates(
      buf: TxnBuffer, decision: MaintenanceAction,
      snapshot: DayStore, rebuilt: DayStore)
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    requires decision != ApplyDelta && decision != RebuildPartition
    ensures StageMaintenance(buf, decision, snapshot, rebuilt).action == decision
    ensures StageMaintenance(buf, decision, snapshot, rebuilt).state == snapshot
  {
  }

  // Exhaustive case split: StageMaintenance outcomes match module.cpp branches.
  lemma StageMaintenanceCaseSplit(buf: TxnBuffer, decision: MaintenanceAction,
                                  snapshot: DayStore, rebuilt: DayStore)
    ensures buf.requires_rebuild ==>
      (StageMaintenance(buf, decision, snapshot, rebuilt).action
        == (if decision == ApplyDelta then RebuildAll else decision))
    ensures !buf.requires_rebuild && |buf.deltas| == 0 ==>
      StageMaintenance(buf, decision, snapshot, rebuilt).action == NoOp
    ensures !buf.requires_rebuild && |buf.deltas| > 0 && decision == ApplyDelta ==>
      (StageMaintenance(buf, decision, snapshot, rebuilt).state
        == ApplyDeltaSeq(snapshot, buf.deltas))
    ensures !buf.requires_rebuild && |buf.deltas| > 0 && decision == RebuildPartition ==>
      StageMaintenance(buf, decision, snapshot, rebuilt).state == rebuilt
  {
  }

  // ----- BuildStateForDays over scan oracles (module.cpp:~516) -----

  datatype DayGroupAggregate = DayGroupAggregate(
    shipdate_days: int,
    returnflag: int,
    linestatus: int,
    count: int,
    sum_qty_raw: int,
    sum_base_price_raw: int,
    sum_discount_raw: int,
    sum_disc_price_raw: int,
    sum_charge_raw: int
  )

  function ConsumeAggregate(state: DayStore, agg: DayGroupAggregate, days_filter: set<int>): DayStore
    requires 0 <= agg.returnflag < 256 && 0 <= agg.linestatus < 256
  {
    if days_filter != {} && agg.shipdate_days !in days_filter then
      state
    else
      var group_key := PackQ1GroupKey(agg.returnflag, agg.linestatus);
      var part := GetPart(state, agg.shipdate_days);
      var group := GetGroup(part, group_key);
      var group2 := Stats(
        group.count + agg.count,
        group.sum_qty + agg.sum_qty_raw,
        group.sum_base_price + agg.sum_base_price_raw,
        group.sum_disc_price + agg.sum_disc_price_raw,
        group.sum_charge + agg.sum_charge_raw,
        group.sum_discount + agg.sum_discount_raw);
      PutPart(state, agg.shipdate_days, PutGroup(part, group_key, group2))
  }

  function ConsumeAggregates(state: DayStore, aggs: seq<DayGroupAggregate>, days_filter: set<int>): DayStore
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    decreases |aggs|
  {
    if |aggs| == 0 then state
    else ConsumeAggregates(ConsumeAggregate(state, aggs[0], days_filter), aggs[1..], days_filter)
  }

  function ConsumeLineitems(state: DayStore, inputs: seq<LineitemInput>, days_filter: set<int>): DayStore
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    decreases |inputs|
  {
    if |inputs| == 0 then
      state
    else if days_filter != {} && inputs[0].shipdate_days !in days_filter then
      ConsumeLineitems(state, inputs[1..], days_filter)
    else
      var day := inputs[0].shipdate_days;
      var part := GetPart(state, day);
      ConsumeLineitems(PutPart(state, day, ApplyLineitemInput(inputs[0], part)), inputs[1..], days_filter)
  }

  // Structural BuildStateForDays: try aggregates-for-days, else aggregates-all, else lineitems.
  function BuildStateForDays(
    days_filter: set<int>,
    aggs_for_days: seq<DayGroupAggregate>,
    used_aggs_for_days: bool,
    aggs_all: seq<DayGroupAggregate>,
    used_aggs_all: bool,
    lineitems: seq<LineitemInput>
  ): DayStore
    requires forall i :: 0 <= i < |aggs_for_days| ==>
              0 <= aggs_for_days[i].returnflag < 256 && 0 <= aggs_for_days[i].linestatus < 256
    requires forall i :: 0 <= i < |aggs_all| ==>
              0 <= aggs_all[i].returnflag < 256 && 0 <= aggs_all[i].linestatus < 256
    requires forall i :: 0 <= i < |lineitems| ==>
              0 <= lineitems[i].returnflag < 256 && 0 <= lineitems[i].linestatus < 256 &&
              lineitems[i].quantity_scale >= 0 && lineitems[i].extended_price_scale >= 0 &&
              lineitems[i].discount_scale >= 0 && lineitems[i].tax_scale >= 0
  {
    if days_filter != {} && used_aggs_for_days then
      ConsumeAggregates(EmptyStore(), aggs_for_days, days_filter)
    else if used_aggs_all then
      ConsumeAggregates(EmptyStore(), aggs_all, days_filter)
    else
      ConsumeLineitems(EmptyStore(), lineitems, days_filter)
  }

  lemma BuildStateLineitemFold(inputs: seq<LineitemInput>)
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures BuildStateForDays({}, [], false, [], false, inputs)
         == ConsumeLineitems(EmptyStore(), inputs, {})
  {
  }

  // ----- ComposeOutput (module.cpp:~553) -----

  function MergeDayInto(merged: DayPart, part: DayPart): DayPart {
    MergeDayIntoFrom(merged, part, 0)
  }

  function MergeDayIntoFrom(merged: DayPart, part: DayPart, i: nat): DayPart
    requires i <= |part.keys|
    decreases |part.keys| - i
  {
    if i == |part.keys| then
      merged
    else if part.keys[i] !in part.groups then
      MergeDayIntoFrom(merged, part, i + 1)
    else
      var k := part.keys[i];
      var s := part.groups[k];
      var m0 := GetGroup(merged, k);
      MergeDayIntoFrom(PutGroup(merged, k, MergeQ1DayGroupStats(m0, s)), part, i + 1)
  }

  function ComposeMergeDays(days: DayStore, cutoff: int, i: nat, merged: DayPart): DayPart
    requires i <= |days.order|
    decreases |days.order| - i
  {
    if i == |days.order| then
      merged
    else
      var day := days.order[i];
      if day > cutoff then
        ComposeMergeDays(days, cutoff, i + 1, merged)
      else
        ComposeMergeDays(days, cutoff, i + 1, MergeDayInto(merged, GetPart(days, day)))
  }

  function ComposeOutput(days: DayStore, cutoff: int): seq<OutRow> {
    var merged := ComposeMergeDays(days, cutoff, 0, EmptyPart());
    // Convert groups to entry seq for Finalize (order of keys is insertion
    // order; FinalizeFromEntries sorts — matches C++ unordered_map iterate + sort).
    FinalizeFromEntries(MapToEntries(merged))
  }

  function MapToEntries(m: DayPart): seq<(int, Stats)> {
    MapToEntriesFrom(m, 0)
  }

  function MapToEntriesFrom(m: DayPart, i: nat): seq<(int, Stats)>
    requires i <= |m.keys|
    decreases |m.keys| - i
  {
    if i == |m.keys| then
      []
    else if m.keys[i] !in m.groups then
      MapToEntriesFrom(m, i + 1)
    else
      [(m.keys[i], m.groups[m.keys[i]])] + MapToEntriesFrom(m, i + 1)
  }

  // ----- LoadPartitions (module.cpp:~568) -----
  // Oracle snapshot images; returns false on typed mismatch (!ok).

  datatype PartImage = PartImage(ok: bool, shipdate_days: int, part: DayPart)

  function LoadPartitionsTwin(cutoff: int, images: seq<PartImage>): (bool, DayStore)
    decreases |images|
  {
    LoadPartitionsTwinRec(cutoff, images, EmptyStore())
  }

  function LoadPartitionsTwinRec(cutoff: int, images: seq<PartImage>, days: DayStore): (bool, DayStore)
    decreases |images|
  {
    if |images| == 0 then
      (true, days)
    else
      var img := images[0];
      if img.shipdate_days > cutoff then
        LoadPartitionsTwinRec(cutoff, images[1..], days)
      else if !img.ok then
        (false, days)
      else
        LoadPartitionsTwinRec(cutoff, images[1..], PutPart(days, img.shipdate_days, img.part))
  }

  // ----- Execute / ExecuteFallback (module.cpp:~472 / ~498) -----

  datatype QueryArgs = QueryArgs(has_offset: bool, day_offset: int)

  function Execute(
    args: QueryArgs,
    base_days: int,
    valid: bool,
    load_ok: bool,
    store_days: DayStore,
    // fallback build
    built: DayStore
  ): seq<OutRow> {
    if !args.has_offset then
      []
    else
      var cutoff := TpchQ1CutoffDays(base_days, args.day_offset);
      if !valid then
        ComposeOutput(built, cutoff)       // ExecuteFallback path
      else if !load_ok then
        ComposeOutput(built, cutoff)
      else
        ComposeOutput(store_days, cutoff)
  }

  function ExecuteFallback(args: QueryArgs, base_days: int, built: DayStore): seq<OutRow> {
    if !args.has_offset then
      []
    else
      ComposeOutput(built, TpchQ1CutoffDays(base_days, args.day_offset))
  }

  lemma ExecuteEqualsFallbackWhenInv(
    args: QueryArgs,
    base_days: int,
    store_days: DayStore,
    built: DayStore
  )
    requires args.has_offset
    requires store_days == built
    ensures Execute(args, base_days, true, true, store_days, built)
         == ExecuteFallback(args, base_days, built)
  {
  }

  lemma ExecuteFallsBackWhenInvalid(args: QueryArgs, base_days: int,
                                    store_days: DayStore, built: DayStore)
    requires args.has_offset
    ensures Execute(args, base_days, false, true, store_days, built)
         == ExecuteFallback(args, base_days, built)
  {
  }

  lemma ExecuteFallsBackWhenLoadFails(args: QueryArgs, base_days: int,
                                      store_days: DayStore, built: DayStore)
    requires args.has_offset
    ensures Execute(args, base_days, true, false, store_days, built)
         == ExecuteFallback(args, base_days, built)
  {
  }

  // ----- EmitResult (module.cpp:~592): row field projection -----

  datatype EmittedRow = EmittedRow(
    returnflag: int,
    linestatus: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    avg_qty: real,
    avg_price: real,
    avg_disc: real,
    count_order: int
  )

  function EmitOne(row: OutRow): EmittedRow
    requires 0 <= row.group_key < 65536
  {
    EmittedRow(
      UnpackQ1Returnflag(row.group_key),
      UnpackQ1Linestatus(row.group_key),
      row.sum_qty,
      row.sum_base_price,
      row.sum_disc_price,
      row.sum_charge,
      row.avg_qty,
      row.avg_price,
      row.avg_disc,
      row.count_order
    )
  }

  function EmitResult(rows: seq<OutRow>): seq<EmittedRow>
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].group_key < 65536
    decreases |rows|
  {
    if |rows| == 0 then []
    else [EmitOne(rows[0])] + EmitResult(rows[1..])
  }

  lemma EmitPreservesAvgs(row: OutRow)
    requires 0 <= row.group_key < 65536
    ensures EmitOne(row).avg_qty == row.avg_qty
    ensures EmitOne(row).avg_price == row.avg_price
    ensures EmitOne(row).avg_disc == row.avg_disc
    ensures EmitOne(row).count_order == row.count_order
  {
  }

  // ----- Encode / Decode abstract payload (sorted entries) -----
  // Byte header/magic are opaque; payload = sorted (gkey, stats) list.

  datatype PartEntry = PartEntry(gkey: int, stats: Stats)

  function EncodeEntries(part: DayPart): seq<PartEntry> {
    EncodeEntriesFrom(part, 0)
  }

  function EncodeEntriesFrom(part: DayPart, i: nat): seq<PartEntry>
    requires i <= |part.keys|
    decreases |part.keys| - i
  {
    if i == |part.keys| then
      []
    else if part.keys[i] !in part.groups then
      EncodeEntriesFrom(part, i + 1)
    else
      InsertEntrySorted(
        EncodeEntriesFrom(part, i + 1),
        PartEntry(part.keys[i], part.groups[part.keys[i]]))
  }

  function InsertEntrySorted(es: seq<PartEntry>, e: PartEntry): seq<PartEntry>
    decreases |es|
  {
    if |es| == 0 then
      [e]
    else if e.gkey < es[0].gkey then
      [e] + es
    else
      [es[0]] + InsertEntrySorted(es[1..], e)
  }

  function DecodeEntries(es: seq<PartEntry>): DayPart
    decreases |es|
  {
    if |es| == 0 then
      EmptyPart()
    else
      PutGroup(DecodeEntries(es[1..]), es[0].gkey, es[0].stats)
  }

  lemma DecodeEncodeSingleton(gkey: int, stats: Stats)
    ensures DecodeEntries([PartEntry(gkey, stats)]) == DayPart(map[gkey := stats], [gkey])
  {
  }
}
