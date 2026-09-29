// Compiled dump of Q1Finalize / Q1ModuleTwin over grid D. Every line,
// including the exact avg(numeric) coefficient/scale lines, must match the
// C++ dump exactly.
// Keep literals in sync with q1_cpp_extract_diff.cpp.
//
// Grid D:
//   flags {A,N,R}×{O,F}  (ASCII 65/78/82 × 79/70)
//   apply (qty,price,disc,tax,sign):
//     (1000,10000,5,4,+1), (100,100,0,0,+1), (5000,100000,10,10,+1)
//     then cancel (1000,10000,5,4,-1) after the first
//   cutoff offsets {0,60,90,120}
//   ScaleToRaw (raw,scale)→target 2: (1000,0), (1000,2), (100000,4)
//   shipdates DaysFromCivil(1995,6,15), (1995,6,16); cutoff+1 excluded
include "../dafny/Q1ModuleTwin.dfy"

import opened Q1Finalize
import opened Q1ModuleTwin

method DumpPack(rf: int, ls: int)
  requires 0 <= rf < 256 && 0 <= ls < 256
{
  var k := PackQ1GroupKey(rf, ls);
  print "pack ", rf, " ", ls, " -> ", k, "\n";
  print "unpack ", k, " rf=", UnpackQ1Returnflag(k), " ls=", UnpackQ1Linestatus(k), "\n";
}

method DumpStats(tag: string, s: Stats)
{
  print tag, " count=", s.count, " qty=", s.sum_qty, " base=", s.sum_base_price,
        " discp=", s.sum_disc_price, " charge=", s.sum_charge, " disc=", s.sum_discount, "\n";
}

method DumpAvg(tag: string, d: PgNumeric.Decimal)
{
  print "avgdec ", tag, " ", d.coeff, " ", d.scale, "\n";
}

method StatsFor(gkey: int, entries: seq<(int, Stats)>) returns (s: Stats)
  decreases |entries|
{
  if |entries| == 0 {
    s := EmptyStats();
  } else if entries[0].0 == gkey {
    s := entries[0].1;
  } else {
    s := StatsFor(gkey, entries[1..]);
  }
}

method DumpOutRow(tag: string, r: OutRow, discount: int)
  requires 0 <= r.group_key < 65536
{
  print tag, " gkey=", r.group_key, " qty=", r.sum_qty, " base=", r.sum_base_price,
        " discp=", r.sum_disc_price, " charge=", r.sum_charge, " count=", r.count_order, "\n";
  DumpAvg(tag + "_qty", r.avg_qty);
  DumpAvg(tag + "_price", r.avg_price);
  DumpAvg(tag + "_disc", r.avg_disc);
}

method DumpFinalized(tag: string, entries: seq<(int, Stats)>)
  requires forall i :: 0 <= i < |entries| ==> 0 <= entries[i].0 < 65536
{
  var rows := FinalizeFromEntries(entries);
  print tag, " nrows=", |rows|, "\n";
  DumpFinalizedFrom(tag, rows, entries);
}

method DumpFinalizedFrom(tag: string, rows: seq<OutRow>, entries: seq<(int, Stats)>)
  requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].group_key < 65536
  decreases |rows|
{
  if |rows| == 0 {
  } else {
    var s := StatsFor(rows[0].group_key, entries);
    DumpOutRow(tag, rows[0], s.sum_discount);
    DumpFinalizedFrom(tag, rows[1..], entries);
  }
}

method DumpTxn(tag: string, b: TxnBuffer)
{
  print tag, " pending=", (if b.pending then 1 else 0),
        " rebuild=", (if b.requires_rebuild then 1 else 0),
        " ndeltas=", |b.deltas|, "\n";
}

method DumpDelta(tag: string, d: LineitemDelta)
{
  print tag, " day=", d.shipdate_days, " gkey=", d.group_key,
        " qty=", d.quantity_raw, " price=", d.extended_price_raw,
        " disc=", d.discount_raw, " tax=", d.tax_raw, " sign=", d.sign, "\n";
}

method DumpDeltas(tag: string, ds: seq<LineitemDelta>)
  decreases |ds|
{
  if |ds| == 0 {
  } else {
    DumpDelta(tag, ds[0]);
    DumpDeltas(tag, ds[1..]);
  }
}

method ScopeName(s: InvalidationScope) returns (n: string)
{
  match s {
    case ScopeNone => n := "None";
    case ScopePartitionSet => n := "PartitionSet";
    case ScopeAll => n := "All";
  }
}

method DumpEntries(tag: string, es: seq<PartEntry>)
  decreases |es|
{
  print tag, " ngroups=", |es|, "\n";
  DumpEntriesFrom(tag, es);
}

method DumpEntriesFrom(tag: string, es: seq<PartEntry>)
  decreases |es|
{
  if |es| == 0 {
  } else {
    var e := es[0];
    print tag, " gkey=", e.gkey, " count=", e.stats.count, " qty=", e.stats.sum_qty,
          " base=", e.stats.sum_base_price, " discp=", e.stats.sum_disc_price,
          " charge=", e.stats.sum_charge, " disc=", e.stats.sum_discount, "\n";
    DumpEntriesFrom(tag, es[1..]);
  }
}

method DumpComposeRows(tag: string, rows: seq<OutRow>, entries: seq<(int, Stats)>)
  requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].group_key < 65536
  decreases |rows|
{
  print tag, " nrows=", |rows|, "\n";
  DumpComposeFrom(tag, rows, entries);
}

method DumpComposeFrom(tag: string, rows: seq<OutRow>, entries: seq<(int, Stats)>)
  requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].group_key < 65536
  decreases |rows|
{
  if |rows| == 0 {
  } else {
    var r := rows[0];
    var s := StatsFor(r.group_key, entries);
    print tag, " rf=", UnpackQ1Returnflag(r.group_key), " ls=", UnpackQ1Linestatus(r.group_key),
          " qty=", r.sum_qty, " base=", r.sum_base_price,
          " discp=", r.sum_disc_price, " charge=", r.sum_charge,
          " count=", r.count_order, "\n";
    DumpAvg(tag + "_qty", r.avg_qty);
    DumpAvg(tag + "_price", r.avg_price);
    DumpAvg(tag + "_disc", r.avg_disc);
    DumpComposeFrom(tag, rows[1..], entries);
  }
}

method DumpPartGroups(tag: string, part: DayPart)
{
  DumpEntries(tag, EncodeEntries(part));
}

method Main()
{
  // ----- Pack / Unpack -----
  DumpPack(65, 79); // A O
  DumpPack(65, 70); // A F
  DumpPack(78, 79); // N O
  DumpPack(78, 70); // N F
  DumpPack(82, 79); // R O
  DumpPack(82, 70); // R F

  // ----- Apply / Merge / Finalize -----
  var s0 := EmptyStats();
  var s1 := ApplyQ1RowContribution(s0, 1000, 10000, 5, 4, 1);
  DumpStats("apply", s1);
  var s2 := ApplyQ1RowContribution(s0, 100, 100, 0, 0, 1);
  DumpStats("apply", s2);
  var s3 := ApplyQ1RowContribution(s0, 5000, 100000, 10, 10, 1);
  DumpStats("apply", s3);
  var sc := ApplyQ1RowContribution(s1, 1000, 10000, 5, 4, -1);
  DumpStats("apply_cancel", sc);

  var merged := MergeQ1DayGroupStats(s1, s2);
  DumpStats("merge", merged);

  var k_no := PackQ1GroupKey(78, 79);
  var k_af := PackQ1GroupKey(65, 70);
  DumpFinalized("finalize", [(k_no, s1), (k_af, s3), (0, EmptyStats())]);

  // ----- Cutoff -----
  print "cutoff offset=0 days=", TpchQ1CutoffDaysFromOffset(0), "\n";
  print "cutoff offset=60 days=", TpchQ1CutoffDaysFromOffset(60), "\n";
  print "cutoff offset=90 days=", TpchQ1CutoffDaysFromOffset(90), "\n";
  print "cutoff offset=120 days=", TpchQ1CutoffDaysFromOffset(120), "\n";

  // ----- ScaleToRaw -----
  print "scale qty raw=1000 scale=0 -> ", ScaleToRaw(1000, 0, 2), "\n";
  print "scale qty raw=1000 scale=2 -> ", ScaleToRaw(1000, 2, 2), "\n";
  print "scale qty raw=100000 scale=4 -> ", ScaleToRaw(100000, 4, 2), "\n";
  print "scale price raw=10000 scale=0 -> ", ScaleToRaw(10000, 0, 2), "\n";
  print "scale price raw=10000 scale=2 -> ", ScaleToRaw(10000, 2, 2), "\n";
  print "scale price raw=1000000 scale=4 -> ", ScaleToRaw(1000000, 4, 2), "\n";

  // ----- TxnBuffer -----
  var ship := DaysFromCivil(1995, 6, 15);
  var d_ins := LineitemDelta(ship, k_no, 1000, 10000, 5, 4, 1);
  var tb := EmptyTxnBuffer();
  DumpTxn("txn_empty", tb);
  tb := TxnAddDelta(tb, d_ins);
  DumpTxn("txn_add", tb);
  tb := TxnMarkRebuild(tb);
  DumpTxn("txn_rebuild", tb);
  tb := TxnClear(tb);
  DumpTxn("txn_clear", tb);

  // ----- InterpretMutation -----
  var sc_li := ScopeName(InterpretMutation(1, 1));
  var sc_ot := ScopeName(InterpretMutation(1, 2));
  print "interpret table=1 scope=", sc_li, "\n";
  print "interpret table=2 scope=", sc_ot, "\n";

  // ----- AccumulateMutation -----
  var dummy := LineitemDelta(0, 0, 0, 0, 0, 0, 0);
  var p_ok := ParsedRow(true, d_ins);
  var p_bad := ParsedRow(false, dummy);

  var a_ins := AccumulateMutation(1, 1, Insert, [], [p_ok], EmptyTxnBuffer());
  DumpTxn("accum_insert", a_ins);
  DumpDeltas("accum_insert", a_ins.deltas);

  var d_del := LineitemDelta(ship, k_no, 1000, 10000, 5, 4, -1);
  var a_del := AccumulateMutation(1, 1, Delete, [ParsedRow(true, d_del)], [], EmptyTxnBuffer());
  DumpTxn("accum_delete", a_del);
  DumpDeltas("accum_delete", a_del.deltas);

  var d_new := LineitemDelta(ship, k_no, 1100, 11000, 6, 4, 1);
  var a_upd := AccumulateMutation(1, 1, Update,
                  [ParsedRow(true, d_del)], [ParsedRow(true, d_new)], EmptyTxnBuffer());
  DumpTxn("accum_update", a_upd);
  DumpDeltas("accum_update", a_upd.deltas);

  var d_day2 := LineitemDelta(ship + 1, k_no, 1100, 11000, 6, 4, 1);
  var a_day := AccumulateMutation(1, 1, Update,
                  [ParsedRow(true, d_del)], [ParsedRow(true, d_day2)], EmptyTxnBuffer());
  DumpTxn("accum_update_day", a_day);

  var a_other := AccumulateMutation(1, 2, Insert, [], [p_ok], EmptyTxnBuffer());
  DumpTxn("accum_other", a_other);

  var a_bad := AccumulateMutation(1, 1, Insert, [], [p_bad], EmptyTxnBuffer());
  DumpTxn("accum_bad", a_bad);

  // ----- Encode / Decode -----
  var in_no := LineitemInput(ship, 1000, 2, 10000, 2, 5, 2, 4, 2, 78, 79);
  var in_af := LineitemInput(ship, 2000, 2, 20000, 2, 6, 2, 3, 2, 65, 70);
  var part := ApplyLineitemInput(in_af, ApplyLineitemInput(in_no, EmptyPart()));
  var enc := EncodeEntries(part);
  DumpEntries("encode", enc);
  DumpEntries("decode", EncodeEntries(DecodeEntries(enc)));

  // ----- Compose + Emit (ExecuteFallback) -----
  var ship2 := DaysFromCivil(1995, 6, 16);
  var past := kQ1BaseDays + 1;
  var in_past := LineitemInput(past, 3000, 2, 30000, 2, 7, 2, 2, 2, 82, 79);
  var in2 := LineitemInput(ship2, 2000, 2, 20000, 2, 6, 2, 3, 2, 65, 70);
  var built := ConsumeLineitems(EmptyStore(), [in_no, in2, in_past], {});
  var cutoff := TpchQ1CutoffDaysFromOffset(0);
  var merged_part := ComposeMergeDays(built, cutoff, 0, EmptyPart());
  var merged_entries := MapToEntries(merged_part);
  var composed := FinalizeFromEntries(merged_entries);
  DumpComposeRows("compose", composed, merged_entries);

  // ----- StageMaintenance ApplyDelta insert then matching delete -----
  var st0 := EmptyStore();
  var st1 := ApplyDeltaToStore(st0, d_ins);
  DumpPartGroups("stage_insert", GetPart(st1, ship));
  var st2 := ApplyDeltaToStore(st1, d_del);
  DumpPartGroups("stage_delete", GetPart(st2, ship));
}
