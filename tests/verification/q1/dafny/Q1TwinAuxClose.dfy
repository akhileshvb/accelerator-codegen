// Q1 Twin↔Aux closing theorems (HB1–HB4).
// Strengthens Q1TwinAuxRefinement; closed Execute + Maint package.
// Intentional SPI residual: AggsFaithful (aggregate-scan oracle ≡ Fold of filtered table).
// Bookkeeping (FinalizeSortFaithful, PartsWellFormed, OrderUnique, BuiltInv from BuildState)
// is proved — not free TCB. Canonical status:
//   tests/verification/q1/CURRENT_THEOREM_TCB.md
// No assume{:axiom}. No C++ change.

include "Q1TwinAuxRefinement.dfy"
include "Q1Maintenance.dfy"
include "Q1ResultSpecBridge.dfy"
include "../../patterns/dafny/SortedUniqueMap.dfy"

module Q1TwinAuxClose {

  import Twin = Q1ModuleTwin
  import F = Q1Finalize
  import opened Q1Aux
  import opened Q1TwinAuxRefinement
  import opened Q1Maintenance
  import Bridge = Q1ResultSpecBridge
  import SU = SortedUniqueMap

  // =====================================================================
  // Well-formed DayPart (keys ↔ groups, unique keys)
  // =====================================================================

  ghost predicate WellFormedPart(part: Twin.DayPart) {
    (forall i :: 0 <= i < |part.keys| ==> part.keys[i] in part.groups) &&
    (forall i, j :: 0 <= i < j < |part.keys| ==> part.keys[i] != part.keys[j]) &&
    (forall g :: g in part.groups ==>
       exists i :: 0 <= i < |part.keys| && part.keys[i] == g)
  }

  lemma EmptyPart_WellFormed()
    ensures WellFormedPart(Twin.EmptyPart())
  {
  }

  lemma PutGroup_preserves_WellFormed(part: Twin.DayPart, gkey: int, stats: F.Stats)
    requires WellFormedPart(part)
    ensures WellFormedPart(Twin.PutGroup(part, gkey, stats))
  {
    var p2 := Twin.PutGroup(part, gkey, stats);
    if gkey in part.groups {
      forall g | g in p2.groups
        ensures exists i :: 0 <= i < |p2.keys| && p2.keys[i] == g
      {
        if g == gkey {
          var i :| 0 <= i < |part.keys| && part.keys[i] == gkey;
          assert p2.keys[i] == gkey;
        } else {
          var i :| 0 <= i < |part.keys| && part.keys[i] == g;
          assert p2.keys[i] == g;
        }
      }
    } else {
      assert p2.keys == part.keys + [gkey];
      forall g | g in p2.groups
        ensures exists i :: 0 <= i < |p2.keys| && p2.keys[i] == g
      {
        if g == gkey {
          assert p2.keys[|part.keys|] == gkey;
        } else {
          var i :| 0 <= i < |part.keys| && part.keys[i] == g;
          assert p2.keys[i] == g;
        }
      }
    }
  }

  lemma GetPart_WellFormed(days: Twin.DayStore, day: int)
    requires forall d :: d in days.parts ==> WellFormedPart(days.parts[d])
    ensures WellFormedPart(Twin.GetPart(days, day))
  {
    if day in days.parts {
    } else {
      EmptyPart_WellFormed();
    }
  }

  lemma FilterKey_removes_only(keys: seq<int>, gkey: int, k: int)
    ensures (k in Twin.FilterKey(keys, gkey)) <==> (k in keys && k != gkey)
    decreases |keys|
  {
    if |keys| == 0 {
    } else if keys[0] == gkey {
      FilterKey_removes_only(keys[1..], gkey, k);
    } else {
      FilterKey_removes_only(keys[1..], gkey, k);
    }
  }

  lemma FilterKey_unique(keys: seq<int>, gkey: int)
    requires forall i, j :: 0 <= i < j < |keys| ==> keys[i] != keys[j]
    ensures forall i, j ::
              0 <= i < j < |Twin.FilterKey(keys, gkey)| ==>
              Twin.FilterKey(keys, gkey)[i] != Twin.FilterKey(keys, gkey)[j]
    decreases |keys|
  {
    if |keys| == 0 {
    } else if keys[0] == gkey {
      FilterKey_unique(keys[1..], gkey);
    } else {
      FilterKey_unique(keys[1..], gkey);
      var fk := Twin.FilterKey(keys, gkey);
      assert fk == [keys[0]] + Twin.FilterKey(keys[1..], gkey);
      forall i, j | 0 <= i < j < |fk|
        ensures fk[i] != fk[j]
      {
        if i == 0 {
          FilterKey_removes_only(keys[1..], gkey, fk[j]);
          assert fk[j] in keys[1..] && fk[j] != gkey;
          assert fk[j] != keys[0];
        } else {
        }
      }
    }
  }

  lemma EraseGroup_preserves_WellFormed(part: Twin.DayPart, gkey: int)
    requires WellFormedPart(part)
    ensures WellFormedPart(Twin.EraseGroup(part, gkey))
  {
    var p2 := Twin.EraseGroup(part, gkey);
    FilterKey_unique(part.keys, gkey);
    forall i | 0 <= i < |p2.keys|
      ensures p2.keys[i] in p2.groups
    {
      FilterKey_removes_only(part.keys, gkey, p2.keys[i]);
      assert p2.keys[i] in part.keys && p2.keys[i] != gkey;
      assert p2.keys[i] in part.groups;
      assert p2.keys[i] in p2.groups;
    }
    forall g | g in p2.groups
      ensures exists i :: 0 <= i < |p2.keys| && p2.keys[i] == g
    {
      assert g in part.groups && g != gkey;
      var i0 :| 0 <= i0 < |part.keys| && part.keys[i0] == g;
      // g appears in FilterKey
      FilterKey_removes_only(part.keys, gkey, g);
      assert g in Twin.FilterKey(part.keys, gkey);
      // witness index in filtered seq
      var i :| 0 <= i < |p2.keys| && p2.keys[i] == g;
    }
  }

  lemma PutPart_preserves_WellFormedDayStore(state: Twin.DayStore, day: int, part: Twin.DayPart)
    requires WellFormedDayStore(state)
    ensures WellFormedDayStore(Twin.PutPart(state, day, part))
  {
  }

  lemma PutPart_preserves_PartsWellFormed(
    state: Twin.DayStore, day: int, part: Twin.DayPart
  )
    requires PartsWellFormed(state)
    requires WellFormedPart(part)
    ensures PartsWellFormed(Twin.PutPart(state, day, part))
  {
  }

  // Package: Twin store bookkeeping invariants.
  ghost predicate PartsWellFormed(days: Twin.DayStore) {
    forall d :: d in days.parts ==> WellFormedPart(days.parts[d])
  }

  ghost predicate OrderUnique(order: seq<int>) {
    forall i, j :: 0 <= i < j < |order| ==> order[i] != order[j]
  }

  ghost predicate TwinStoreOk(days: Twin.DayStore) {
    WellFormedDayStore(days) &&
    PartsWellFormed(days) &&
    OrderUnique(days.order)
  }

  lemma Empty_TwinStoreOk()
    ensures TwinStoreOk(Twin.EmptyStore())
  {
    EmptyPart_WellFormed();
    Empty_WellFormed();
  }

  lemma PutPart_preserves_OrderUnique(state: Twin.DayStore, day: int, part: Twin.DayPart)
    requires OrderUnique(state.order)
    requires day in state.parts || day !in state.order
    ensures OrderUnique(Twin.PutPart(state, day, part).order)
  {
    if day in state.parts {
    } else {
      forall i, j | 0 <= i < j < |Twin.PutPart(state, day, part).order|
        ensures Twin.PutPart(state, day, part).order[i]
             != Twin.PutPart(state, day, part).order[j]
      {
        var ord := Twin.PutPart(state, day, part).order;
        assert ord == state.order + [day];
        if j < |state.order| {
        } else {
          assert ord[j] == day;
          assert i < |state.order|;
          assert ord[i] != day;
        }
      }
    }
  }

  // Stronger: order lists exactly the days in parts (PutPart discipline from Empty).
  ghost predicate OrderExact(days: Twin.DayStore) {
    WellFormedDayStore(days) &&
    OrderUnique(days.order) &&
    (forall i :: 0 <= i < |days.order| ==> days.order[i] in days.parts) &&
    (forall d :: d in days.parts ==> d in days.order)
  }

  lemma Empty_OrderExact()
    ensures OrderExact(Twin.EmptyStore())
  {
    Empty_WellFormed();
  }

  lemma PutPart_preserves_OrderExact(state: Twin.DayStore, day: int, part: Twin.DayPart)
    requires OrderExact(state)
    ensures OrderExact(Twin.PutPart(state, day, part))
  {
    if day !in state.parts {
      assert day !in state.order;
    }
    PutPart_preserves_OrderUnique(state, day, part);
    PutPart_preserves_WellFormedDayStore(state, day, part);
  }

  lemma ApplyDeltaToStore_preserves_TwinStoreOk(state: Twin.DayStore, d: Twin.LineitemDelta)
    requires TwinStoreOk(state)
    requires OrderExact(state)
    ensures TwinStoreOk(Twin.ApplyDeltaToStore(state, d))
    ensures OrderExact(Twin.ApplyDeltaToStore(state, d))
  {
    var day := d.shipdate_days;
    var part := Twin.GetPart(state, day);
    GetPart_WellFormed(state, day);
    var group := Twin.GetGroup(part, d.group_key);
    var group2 := F.ApplyQ1RowContribution(
      group, d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw, d.sign);
    if group2.count == 0 {
      var part2 := Twin.EraseGroup(part, d.group_key);
      EraseGroup_preserves_WellFormed(part, d.group_key);
      PutPart_preserves_PartsWellFormed(state, day, part2);
      PutPart_preserves_OrderExact(state, day, part2);
      ApplyDeltaToStore_preserves_WellFormed(state, d);
    } else {
      var part2 := Twin.PutGroup(part, d.group_key, group2);
      PutGroup_preserves_WellFormed(part, d.group_key, group2);
      PutPart_preserves_PartsWellFormed(state, day, part2);
      PutPart_preserves_OrderExact(state, day, part2);
      ApplyDeltaToStore_preserves_WellFormed(state, d);
    }
  }

  lemma ApplyDeltaSeq_preserves_TwinStoreOk(state: Twin.DayStore, ds: seq<Twin.LineitemDelta>)
    requires TwinStoreOk(state)
    requires OrderExact(state)
    ensures TwinStoreOk(Twin.ApplyDeltaSeq(state, ds))
    ensures OrderExact(Twin.ApplyDeltaSeq(state, ds))
    decreases |ds|
  {
    if |ds| == 0 {
    } else {
      ApplyDeltaToStore_preserves_TwinStoreOk(state, ds[0]);
      ApplyDeltaSeq_preserves_TwinStoreOk(Twin.ApplyDeltaToStore(state, ds[0]), ds[1..]);
    }
  }

  lemma ApplyLineitemInput_preserves_WellFormed(part: Twin.DayPart, inp: Twin.LineitemInput)
    requires WellFormedPart(part)
    requires 0 <= inp.returnflag < 256 && 0 <= inp.linestatus < 256
    requires inp.quantity_scale >= 0 && inp.extended_price_scale >= 0
    requires inp.discount_scale >= 0 && inp.tax_scale >= 0
    ensures WellFormedPart(Twin.ApplyLineitemInput(inp, part))
  {
    var gkey := F.PackQ1GroupKey(inp.returnflag, inp.linestatus);
    var g := Twin.GetGroup(part, gkey);
    var g2 := F.ApplyQ1RowContribution(
      g,
      Twin.ScaleToRaw(inp.quantity_raw, inp.quantity_scale, 2),
      Twin.ScaleToRaw(inp.extended_price_raw, inp.extended_price_scale, 2),
      Twin.ScaleToRaw(inp.discount_raw, inp.discount_scale, 2),
      Twin.ScaleToRaw(inp.tax_raw, inp.tax_scale, 2),
      1);
    PutGroup_preserves_WellFormed(part, gkey, g2);
  }

  lemma ConsumeLineitems_preserves_TwinStoreOk(
    state: Twin.DayStore, inputs: seq<Twin.LineitemInput>, days_filter: set<int>
  )
    requires TwinStoreOk(state)
    requires OrderExact(state)
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures TwinStoreOk(Twin.ConsumeLineitems(state, inputs, days_filter))
    ensures OrderExact(Twin.ConsumeLineitems(state, inputs, days_filter))
    decreases |inputs|
  {
    if |inputs| == 0 {
    } else if days_filter != {} && inputs[0].shipdate_days !in days_filter {
      ConsumeLineitems_preserves_TwinStoreOk(state, inputs[1..], days_filter);
    } else {
      var day := inputs[0].shipdate_days;
      var part := Twin.GetPart(state, day);
      GetPart_WellFormed(state, day);
      ApplyLineitemInput_preserves_WellFormed(part, inputs[0]);
      var part2 := Twin.ApplyLineitemInput(inputs[0], part);
      PutPart_preserves_PartsWellFormed(state, day, part2);
      PutPart_preserves_OrderExact(state, day, part2);
      PutPart_preserves_WellFormedDayStore(state, day, part2);
      ConsumeLineitems_preserves_TwinStoreOk(
        Twin.PutPart(state, day, part2), inputs[1..], days_filter);
    }
  }

  lemma ConsumeAggregate_preserves_TwinStoreOk(
    state: Twin.DayStore, agg: Twin.DayGroupAggregate, days_filter: set<int>
  )
    requires TwinStoreOk(state)
    requires OrderExact(state)
    requires 0 <= agg.returnflag < 256 && 0 <= agg.linestatus < 256
    ensures TwinStoreOk(Twin.ConsumeAggregate(state, agg, days_filter))
    ensures OrderExact(Twin.ConsumeAggregate(state, agg, days_filter))
  {
    if days_filter != {} && agg.shipdate_days !in days_filter {
    } else {
      var day := agg.shipdate_days;
      var gkey := F.PackQ1GroupKey(agg.returnflag, agg.linestatus);
      var part := Twin.GetPart(state, day);
      GetPart_WellFormed(state, day);
      var group := Twin.GetGroup(part, gkey);
      var group2 := F.Stats(
        group.count + agg.count,
        group.sum_qty + agg.sum_qty_raw,
        group.sum_base_price + agg.sum_base_price_raw,
        group.sum_disc_price + agg.sum_disc_price_raw,
        group.sum_charge + agg.sum_charge_raw,
        group.sum_discount + agg.sum_discount_raw);
      PutGroup_preserves_WellFormed(part, gkey, group2);
      var part2 := Twin.PutGroup(part, gkey, group2);
      PutPart_preserves_PartsWellFormed(state, day, part2);
      PutPart_preserves_OrderExact(state, day, part2);
      PutPart_preserves_WellFormedDayStore(state, day, part2);
    }
  }

  lemma ConsumeAggregates_preserves_TwinStoreOk(
    state: Twin.DayStore, aggs: seq<Twin.DayGroupAggregate>, days_filter: set<int>
  )
    requires TwinStoreOk(state)
    requires OrderExact(state)
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    ensures TwinStoreOk(Twin.ConsumeAggregates(state, aggs, days_filter))
    ensures OrderExact(Twin.ConsumeAggregates(state, aggs, days_filter))
    decreases |aggs|
  {
    if |aggs| == 0 {
    } else {
      ConsumeAggregate_preserves_TwinStoreOk(state, aggs[0], days_filter);
      ConsumeAggregates_preserves_TwinStoreOk(
        Twin.ConsumeAggregate(state, aggs[0], days_filter), aggs[1..], days_filter);
    }
  }

  lemma BuildStateForDays_preserves_TwinStoreOk(
    days_filter: set<int>,
    aggs_for_days: seq<Twin.DayGroupAggregate>,
    used_aggs_for_days: bool,
    aggs_all: seq<Twin.DayGroupAggregate>,
    used_aggs_all: bool,
    lineitems: seq<Twin.LineitemInput>
  )
    requires forall i :: 0 <= i < |aggs_for_days| ==>
              0 <= aggs_for_days[i].returnflag < 256 && 0 <= aggs_for_days[i].linestatus < 256
    requires forall i :: 0 <= i < |aggs_all| ==>
              0 <= aggs_all[i].returnflag < 256 && 0 <= aggs_all[i].linestatus < 256
    requires forall i :: 0 <= i < |lineitems| ==>
              0 <= lineitems[i].returnflag < 256 && 0 <= lineitems[i].linestatus < 256 &&
              lineitems[i].quantity_scale >= 0 && lineitems[i].extended_price_scale >= 0 &&
              lineitems[i].discount_scale >= 0 && lineitems[i].tax_scale >= 0
    ensures TwinStoreOk(Twin.BuildStateForDays(
              days_filter, aggs_for_days, used_aggs_for_days,
              aggs_all, used_aggs_all, lineitems))
    ensures OrderExact(Twin.BuildStateForDays(
              days_filter, aggs_for_days, used_aggs_for_days,
              aggs_all, used_aggs_all, lineitems))
  {
    Empty_TwinStoreOk();
    Empty_OrderExact();
    if days_filter != {} && used_aggs_for_days {
      ConsumeAggregates_preserves_TwinStoreOk(Twin.EmptyStore(), aggs_for_days, days_filter);
    } else if used_aggs_all {
      ConsumeAggregates_preserves_TwinStoreOk(Twin.EmptyStore(), aggs_all, days_filter);
    } else {
      ConsumeLineitems_preserves_TwinStoreOk(Twin.EmptyStore(), lineitems, days_filter);
    }
  }

  lemma DecodeEntries_preserves_WellFormed(es: seq<Twin.PartEntry>)
    requires forall i, j :: 0 <= i < j < |es| ==> es[i].gkey != es[j].gkey
    ensures WellFormedPart(Twin.DecodeEntries(es))
    decreases |es|
  {
    if |es| == 0 {
      EmptyPart_WellFormed();
    } else {
      DecodeEntries_preserves_WellFormed(es[1..]);
      PutGroup_preserves_WellFormed(Twin.DecodeEntries(es[1..]), es[0].gkey, es[0].stats);
    }
  }

  lemma LoadPartitionsTwin_preserves_TwinStoreOk(
    cutoff: int, images: seq<Twin.PartImage>
  )
    requires forall i :: 0 <= i < |images| && images[i].ok ==>
              WellFormedPart(images[i].part)
    requires forall i, j :: 0 <= i < j < |images| && images[i].ok && images[j].ok ==>
              images[i].shipdate_days != images[j].shipdate_days
    ensures var r := Twin.LoadPartitionsTwin(cutoff, images);
            r.0 ==> TwinStoreOk(r.1) && OrderExact(r.1)
  {
    LoadPartitionsTwinRec_preserves(cutoff, images, Twin.EmptyStore());
  }

  lemma LoadPartitionsTwinRec_preserves(
    cutoff: int, images: seq<Twin.PartImage>, days: Twin.DayStore
  )
    requires TwinStoreOk(days)
    requires OrderExact(days)
    requires forall i :: 0 <= i < |images| && images[i].ok ==>
              WellFormedPart(images[i].part)
    requires forall i :: 0 <= i < |images| && images[i].ok ==>
              images[i].shipdate_days !in days.parts
    requires forall i, j :: 0 <= i < j < |images| && images[i].ok && images[j].ok ==>
              images[i].shipdate_days != images[j].shipdate_days
    ensures var r := Twin.LoadPartitionsTwinRec(cutoff, images, days);
            r.0 ==> TwinStoreOk(r.1) && OrderExact(r.1)
    decreases |images|
  {
    Empty_TwinStoreOk();
    if |images| == 0 {
    } else {
      var img := images[0];
      if img.shipdate_days > cutoff {
        LoadPartitionsTwinRec_preserves(cutoff, images[1..], days);
      } else if !img.ok {
      } else {
        PutPart_preserves_PartsWellFormed(days, img.shipdate_days, img.part);
        PutPart_preserves_OrderExact(days, img.shipdate_days, img.part);
        PutPart_preserves_WellFormedDayStore(days, img.shipdate_days, img.part);
        var days1 := Twin.PutPart(days, img.shipdate_days, img.part);
        LoadPartitionsTwinRec_preserves(cutoff, images[1..], days1);
      }
    }
  }

  // =====================================================================
  // HB3b map — MergeDayInto / ComposeMergeDays GetGroup characterization
  // =====================================================================

  lemma Merge_Empty_right(s: F.Stats)
    ensures F.MergeQ1DayGroupStats(s, F.EmptyStats()) == s
  {
  }

  lemma Merge_Empty_left(s: F.Stats)
    ensures F.MergeQ1DayGroupStats(F.EmptyStats(), s) == s
  {
  }

  // Contrib of keys[j..] toward group g (unique key ⇒ at most one hit).
  ghost function PartContribFrom(part: Twin.DayPart, g: int, j: nat): F.Stats
    requires j <= |part.keys|
    decreases |part.keys| - j
  {
    if j == |part.keys| then
      F.EmptyStats()
    else if part.keys[j] !in part.groups then
      PartContribFrom(part, g, j + 1)
    else if part.keys[j] == g then
      F.MergeQ1DayGroupStats(part.groups[g], PartContribFrom(part, g, j + 1))
    else
      PartContribFrom(part, g, j + 1)
  }

  lemma PartContribFrom_eq_GetGroup(part: Twin.DayPart, g: int)
    requires WellFormedPart(part)
    ensures PartContribFrom(part, g, 0) == Twin.GetGroup(part, g)
  {
    if g in part.groups {
      PartContribFrom_hits_once(part, g, 0);
    } else {
      PartContribFrom_miss(part, g, 0);
    }
  }

  lemma PartContribFrom_miss(part: Twin.DayPart, g: int, j: nat)
    requires WellFormedPart(part)
    requires g !in part.groups
    requires j <= |part.keys|
    ensures PartContribFrom(part, g, j) == F.EmptyStats()
    decreases |part.keys| - j
  {
    if j == |part.keys| {
    } else if part.keys[j] !in part.groups {
      PartContribFrom_miss(part, g, j + 1);
    } else if part.keys[j] == g {
      assert false;  // keys[j] in groups and == g ⇒ g in groups
    } else {
      PartContribFrom_miss(part, g, j + 1);
    }
  }

  lemma PartContribFrom_hits_once(part: Twin.DayPart, g: int, j: nat)
    requires WellFormedPart(part)
    requires g in part.groups
    requires j <= |part.keys|
    requires forall i :: 0 <= i < j ==> part.keys[i] != g
    ensures PartContribFrom(part, g, j) == part.groups[g]
    decreases |part.keys| - j
  {
    if j == |part.keys| {
      // g in groups ⇒ some key equals g, contradiction with forall i < |keys|
      var i :| 0 <= i < |part.keys| && part.keys[i] == g;
      assert false;
    } else if part.keys[j] !in part.groups {
      assert false;  // WellFormed: keys[j] in groups
    } else if part.keys[j] == g {
      // rest must miss g by uniqueness
      PartContribFrom_miss_after(part, g, j + 1);
      Merge_Empty_right(part.groups[g]);
      assert PartContribFrom(part, g, j)
          == F.MergeQ1DayGroupStats(part.groups[g], PartContribFrom(part, g, j + 1));
      assert PartContribFrom(part, g, j + 1) == F.EmptyStats();
    } else {
      PartContribFrom_hits_once(part, g, j + 1);
    }
  }

  lemma PartContribFrom_miss_after(part: Twin.DayPart, g: int, j: nat)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    requires forall i :: j <= i < |part.keys| ==> part.keys[i] != g
    ensures PartContribFrom(part, g, j) == F.EmptyStats()
    decreases |part.keys| - j
  {
    if j == |part.keys| {
    } else if part.keys[j] !in part.groups {
      PartContribFrom_miss_after(part, g, j + 1);
    } else if part.keys[j] == g {
      assert false;
    } else {
      PartContribFrom_miss_after(part, g, j + 1);
    }
  }

  lemma {:isolate_assertions} MergeDayIntoFrom_GetGroup(
    merged: Twin.DayPart, part: Twin.DayPart, j: nat, g: int
  )
    requires WellFormedPart(merged)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    ensures WellFormedPart(Twin.MergeDayIntoFrom(merged, part, j))
    ensures Twin.GetGroup(Twin.MergeDayIntoFrom(merged, part, j), g)
         == F.MergeQ1DayGroupStats(Twin.GetGroup(merged, g), PartContribFrom(part, g, j))
    decreases |part.keys| - j
  {
    if j == |part.keys| {
      Merge_Empty_right(Twin.GetGroup(merged, g));
    } else if part.keys[j] !in part.groups {
      assert false;
    } else {
      var k := part.keys[j];
      var s := part.groups[k];
      var m0 := Twin.GetGroup(merged, k);
      var merged1 := Twin.PutGroup(merged, k, F.MergeQ1DayGroupStats(m0, s));
      PutGroup_preserves_WellFormed(merged, k, F.MergeQ1DayGroupStats(m0, s));
      MergeDayIntoFrom_GetGroup(merged1, part, j + 1, g);
      if k == g {
        // GetGroup(merged1, g) == Merge(GetGroup(merged,g), s)
        assert Twin.GetGroup(merged1, g) == F.MergeQ1DayGroupStats(m0, s);
        assert m0 == Twin.GetGroup(merged, g);
        assert s == part.groups[g];
        // PartContribFrom(part,g,j) == Merge(s, PartContribFrom(j+1))
        F.MergeAssociative(Twin.GetGroup(merged, g), s, PartContribFrom(part, g, j + 1));
      } else {
        assert Twin.GetGroup(merged1, g) == Twin.GetGroup(merged, g);
      }
    }
  }

  lemma MergeDayInto_GetGroup(merged: Twin.DayPart, part: Twin.DayPart, g: int)
    requires WellFormedPart(merged)
    requires WellFormedPart(part)
    ensures WellFormedPart(Twin.MergeDayInto(merged, part))
    ensures Twin.GetGroup(Twin.MergeDayInto(merged, part), g)
         == F.MergeQ1DayGroupStats(Twin.GetGroup(merged, g), Twin.GetGroup(part, g))
  {
    MergeDayIntoFrom_GetGroup(merged, part, 0, g);
    PartContribFrom_eq_GetGroup(part, g);
  }

  ghost function TwinMergeGroupAlongOrder(
    days: Twin.DayStore, cutoff: int, g: int, i: nat, acc: F.Stats
  ): F.Stats
    requires i <= |days.order|
    decreases |days.order| - i
  {
    if i == |days.order| then
      acc
    else if days.order[i] > cutoff then
      TwinMergeGroupAlongOrder(days, cutoff, g, i + 1, acc)
    else
      var part := Twin.GetPart(days, days.order[i]);
      TwinMergeGroupAlongOrder(days, cutoff, g, i + 1,
        F.MergeQ1DayGroupStats(acc, Twin.GetGroup(part, g)))
  }

  lemma {:isolate_assertions} ComposeMergeDays_GetGroup(
    days: Twin.DayStore, cutoff: int, i: nat, merged: Twin.DayPart, g: int
  )
    requires i <= |days.order|
    requires WellFormedPart(merged)
    requires forall d :: d in days.parts ==> WellFormedPart(days.parts[d])
    ensures WellFormedPart(Twin.ComposeMergeDays(days, cutoff, i, merged))
    ensures Twin.GetGroup(Twin.ComposeMergeDays(days, cutoff, i, merged), g)
         == TwinMergeGroupAlongOrder(days, cutoff, g, i, Twin.GetGroup(merged, g))
    decreases |days.order| - i
  {
    if i == |days.order| {
    } else if days.order[i] > cutoff {
      ComposeMergeDays_GetGroup(days, cutoff, i + 1, merged, g);
    } else {
      var day := days.order[i];
      var part := Twin.GetPart(days, day);
      GetPart_WellFormed(days, day);
      var merged1 := Twin.MergeDayInto(merged, part);
      MergeDayInto_GetGroup(merged, part, g);
      ComposeMergeDays_GetGroup(days, cutoff, i + 1, merged1, g);
    }
  }

  lemma TwinMerged_GetGroup_eq_TwinMergeAlong(
    days: Twin.DayStore, cutoff: int, g: int
  )
    requires forall d :: d in days.parts ==> WellFormedPart(days.parts[d])
    ensures Twin.GetGroup(TwinMergedAtCutoff(days, cutoff), g)
         == TwinMergeGroupAlongOrder(days, cutoff, g, 0, F.EmptyStats())
  {
    EmptyPart_WellFormed();
    ComposeMergeDays_GetGroup(days, cutoff, 0, Twin.EmptyPart(), g);
    assert Twin.GetGroup(Twin.EmptyPart(), g) == F.EmptyStats();
  }

  // =====================================================================
  // TwinMergeAlong ≡ AuxMergeAlong under Flatten
  // =====================================================================

  ghost function AuxMergeGroupAlongOrder(
    store: Store, cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats
  ): Stats
    decreases |order| - i
  {
    if i >= |order| then
      acc
    else if order[i] > cutoff then
      AuxMergeGroupAlongOrder(store, cutoff, order, g, i + 1, acc)
    else
      AuxMergeGroupAlongOrder(store, cutoff, order, g, i + 1,
        Merge(acc, Get(store, CellKey(order[i], g))))
  }

  lemma StatsToAux_Merge(a: F.Stats, b: F.Stats)
    ensures StatsToAux(F.MergeQ1DayGroupStats(a, b))
         == Merge(StatsToAux(a), StatsToAux(b))
  {
  }

  lemma TwinMergeGroup_eq_AuxMergeGroup(
    days: Twin.DayStore, cutoff: int, g: int, i: nat, acc: F.Stats
  )
    requires i <= |days.order|
    ensures StatsToAux(TwinMergeGroupAlongOrder(days, cutoff, g, i, acc))
         == AuxMergeGroupAlongOrder(Flatten(days), cutoff, days.order, g, i, StatsToAux(acc))
    decreases |days.order| - i
  {
    if i == |days.order| {
    } else if days.order[i] > cutoff {
      TwinMergeGroup_eq_AuxMergeGroup(days, cutoff, g, i + 1, acc);
    } else {
      var day := days.order[i];
      var part := Twin.GetPart(days, day);
      var gstats := Twin.GetGroup(part, g);
      StatsToAux_Merge(acc, gstats);
      if day in days.parts && g in days.parts[day].groups {
        Flatten_lookup(days, day, g);
        assert Get(Flatten(days), CellKey(day, g)) == StatsToAux(gstats);
      } else {
        Flatten_absent(days, day, g);
        assert StatsToAux(gstats) == EmptyStats();
        assert Get(Flatten(days), CellKey(day, g)) == EmptyStats();
      }
      TwinMergeGroup_eq_AuxMergeGroup(days, cutoff, g, i + 1,
        F.MergeQ1DayGroupStats(acc, gstats));
    }
  }

  // =====================================================================
  // ----- AuxMergeAlong(Fold(t)) ≡ FilteredContrib under OrderCoversFoldDays -----

  // Runtime / structural contract: every day that contributes to Fold for g
  // under the cutoff appears in `order` (Twin WellFormedDayStore supplies this).
  ghost predicate OrderCoversFoldDays(t: seq<Row>, cutoff: int, g: int, order: seq<int>)
    requires RowsOk(t)
  {
    forall d :: CellKey(d, g) in Fold(t) && d <= cutoff ==> d in order
  }

  lemma Merge_EmptyStats_right(s: Stats)
    ensures Merge(s, EmptyStats()) == s
  {
  }

  lemma Merge_EmptyStats_left(s: Stats)
    ensures Merge(EmptyStats(), s) == s
  {
  }

  lemma Apply_eq_Merge_row(s: Stats, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures ApplyRow(s, r, 1) == Merge(s, ApplyRow(EmptyStats(), r, 1))
  {
  }

  lemma AuxMerge_empty_store(cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats)
    requires i <= |order|
    ensures AuxMergeGroupAlongOrder(map[], cutoff, order, g, i, acc) == acc
    decreases |order| - i
  {
    if i >= |order| {
    } else if order[i] > cutoff {
      AuxMerge_empty_store(cutoff, order, g, i + 1, acc);
    } else {
      Merge_EmptyStats_right(acc);
      AuxMerge_empty_store(cutoff, order, g, i + 1, acc);
    }
  }

  lemma Get_ApplyDelta_same_cell(store: Store, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires Get(store, CellKey(r.shipdate, RowKey(r))).count >= 0
    ensures Get(ApplyDelta(store, r, 1), CellKey(r.shipdate, RowKey(r)))
         == ApplyRow(Get(store, CellKey(r.shipdate, RowKey(r))), r, 1)
  {
    var k := CellKey(r.shipdate, RowKey(r));
    var s2 := ApplyRow(Get(store, k), r, 1);
    assert s2.count == Get(store, k).count + 1 >= 1;
    assert ApplyDelta(store, r, 1) == store[k := s2];
  }

  lemma Get_ApplyDelta_other_cell(store: Store, r: Row, d: int, g: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires CellKey(d, g) != CellKey(r.shipdate, RowKey(r))
    ensures Get(ApplyDelta(store, r, 1), CellKey(d, g)) == Get(store, CellKey(d, g))
  {
  }

  lemma Fold_Get_count_nonneg(t: seq<Row>, d: int, g: int)
    requires RowsOk(t)
    ensures Get(Fold(t), CellKey(d, g)).count >= 0
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      Fold_Get_count_nonneg(pref, d, g);
      FoldStep(pref, r);
      var k := CellKey(r.shipdate, RowKey(r));
      if CellKey(d, g) == k {
        var s0 := Get(Fold(pref), k);
        assert s0.count >= 0;
        var s2 := ApplyRow(s0, r, 1);
        assert s2.count == s0.count + 1 >= 1;
        assert Get(Fold(t), k) == s2;
      } else {
        Get_ApplyDelta_other_cell(Fold(pref), r, d, g);
      }
    }
  }

  lemma {:isolate_assertions} AuxMerge_ApplyDelta_unchanged(
    store: Store, r: Row, cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats
  )
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires i <= |order|
    requires !(r.shipdate <= cutoff && RowKey(r) == g)
    ensures AuxMergeGroupAlongOrder(ApplyDelta(store, r, 1), cutoff, order, g, i, acc)
         == AuxMergeGroupAlongOrder(store, cutoff, order, g, i, acc)
    decreases |order| - i
  {
    if i >= |order| {
    } else if order[i] > cutoff {
      AuxMerge_ApplyDelta_unchanged(store, r, cutoff, order, g, i + 1, acc);
    } else {
      var d := order[i];
      assert CellKey(d, g) != CellKey(r.shipdate, RowKey(r));
      Get_ApplyDelta_other_cell(store, r, d, g);
      AuxMerge_ApplyDelta_unchanged(store, r, cutoff, order, g, i + 1,
        Merge(acc, Get(store, CellKey(d, g))));
    }
  }

  // After the contributing day has been consumed, stores that differ only at
  // that cell agree on AuxMerge for the remaining order suffix.
  lemma {:isolate_assertions} AuxMerge_agree_away_from_day(
    store_new: Store, store_old: Store, r: Row,
    cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats
  )
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires i <= |order|
    requires RowKey(r) == g
    requires store_new == ApplyDelta(store_old, r, 1)
    requires forall j :: i <= j < |order| ==> order[j] != r.shipdate
    ensures AuxMergeGroupAlongOrder(store_new, cutoff, order, g, i, acc)
         == AuxMergeGroupAlongOrder(store_old, cutoff, order, g, i, acc)
    decreases |order| - i
  {
    if i >= |order| {
    } else if order[i] > cutoff {
      AuxMerge_agree_away_from_day(store_new, store_old, r, cutoff, order, g, i + 1, acc);
    } else {
      assert order[i] != r.shipdate;
      Get_ApplyDelta_other_cell(store_old, r, order[i], g);
      AuxMerge_agree_away_from_day(store_new, store_old, r, cutoff, order, g, i + 1,
        Merge(acc, Get(store_old, CellKey(order[i], g))));
    }
  }

  lemma AuxMerge_acc_Merge_row(
    store: Store, cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats, r: Row
  )
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires i <= |order|
    ensures AuxMergeGroupAlongOrder(store, cutoff, order, g, i,
              Merge(acc, ApplyRow(EmptyStats(), r, 1)))
         == Merge(AuxMergeGroupAlongOrder(store, cutoff, order, g, i, acc),
                  ApplyRow(EmptyStats(), r, 1))
    decreases |order| - i
  {
    if i >= |order| {
    } else if order[i] > cutoff {
      AuxMerge_acc_Merge_row(store, cutoff, order, g, i + 1, acc, r);
    } else {
      var cell := Get(store, CellKey(order[i], g));
      MergeAssociative(acc, ApplyRow(EmptyStats(), r, 1), cell);
      MergeCommutative(ApplyRow(EmptyStats(), r, 1), cell);
      MergeAssociative(acc, cell, ApplyRow(EmptyStats(), r, 1));
      AuxMerge_acc_Merge_row(store, cutoff, order, g, i + 1, Merge(acc, cell), r);
    }
  }

  // Contributing row: AuxMerge after ApplyDelta = Merge(AuxMerge before, row contrib).
  // Requires the shipdate still appears in order[i..].
  lemma {:isolate_assertions} AuxMerge_ApplyDelta_contrib(
    store: Store, r: Row, cutoff: int, order: seq<int>, g: int, i: nat, acc: Stats
  )
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires i <= |order|
    requires r.shipdate <= cutoff && RowKey(r) == g
    requires exists j :: i <= j < |order| && order[j] == r.shipdate
    requires OrderUnique(order)
    requires Get(store, CellKey(r.shipdate, g)).count >= 0
    ensures AuxMergeGroupAlongOrder(ApplyDelta(store, r, 1), cutoff, order, g, i, acc)
         == Merge(
              AuxMergeGroupAlongOrder(store, cutoff, order, g, i, acc),
              ApplyRow(EmptyStats(), r, 1))
    decreases |order| - i
  {
    if i >= |order| {
      assert exists j :: i <= j < |order| && order[j] == r.shipdate;
      assert false;
    } else if order[i] > cutoff {
      // r.shipdate <= cutoff < order[i], so the witness j > i
      AuxMerge_ApplyDelta_contrib(store, r, cutoff, order, g, i + 1, acc);
    } else if order[i] == r.shipdate {
      Get_ApplyDelta_same_cell(store, r);
      var s_old := Get(store, CellKey(r.shipdate, g));
      var s_new := ApplyRow(s_old, r, 1);
      var rowc := ApplyRow(EmptyStats(), r, 1);
      Apply_eq_Merge_row(s_old, r);
      assert s_new == Merge(s_old, rowc);
      // Unique ⇒ no later occurrence of r.shipdate
      assert forall j :: i < j < |order| ==> order[j] != r.shipdate;
      AuxMerge_agree_away_from_day(
        ApplyDelta(store, r, 1), store, r, cutoff, order, g, i + 1, Merge(acc, s_new));
      // AuxMerge(new, i+1, Merge(acc,s_new)) == AuxMerge(old, i+1, Merge(acc,s_new))
      assert AuxMergeGroupAlongOrder(ApplyDelta(store, r, 1), cutoff, order, g, i + 1, Merge(acc, s_new))
          == AuxMergeGroupAlongOrder(store, cutoff, order, g, i + 1, Merge(acc, s_new));
      MergeAssociative(acc, s_old, rowc);
      assert Merge(acc, s_new) == Merge(Merge(acc, s_old), rowc);
      AuxMerge_acc_Merge_row(store, cutoff, order, g, i + 1, Merge(acc, s_old), r);
      assert AuxMergeGroupAlongOrder(store, cutoff, order, g, i + 1, Merge(acc, s_new))
          == Merge(AuxMergeGroupAlongOrder(store, cutoff, order, g, i + 1, Merge(acc, s_old)), rowc);
      // Left side of goal at i: AuxMerge(new, i, acc) = AuxMerge(new, i+1, Merge(acc,s_new))
      // Right: Merge(AuxMerge(old, i, acc), rowc) = Merge(AuxMerge(old, i+1, Merge(acc,s_old)), rowc)
    } else {
      Get_ApplyDelta_other_cell(store, r, order[i], g);
      // witness j != i, so j >= i+1
      AuxMerge_ApplyDelta_contrib(store, r, cutoff, order, g, i + 1,
        Merge(acc, Get(store, CellKey(order[i], g))));
    }
  }

  lemma OrderCovers_preserves_snoc(t: seq<Row>, r: Row, cutoff: int, g: int, order: seq<int>)
    requires RowsOk(t)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires OrderCoversFoldDays(t + [r], cutoff, g, order)
    ensures OrderCoversFoldDays(t, cutoff, g, order)
  {
    forall d | CellKey(d, g) in Fold(t) && d <= cutoff
      ensures d in order
    {
      FoldStep(t, r);
      // Cell in Fold(t) stays in Fold(t+[r]) unless erased — erase only on count 0
      // after Apply with +1, never erases an existing positive/absent→positive cell
      // Absent cell in Fold(t): not in domain. Present: ApplyDelta keeps or updates.
      if CellKey(d, g) == CellKey(r.shipdate, RowKey(r)) {
        var s0 := Get(Fold(t), CellKey(d, g));
        Fold_Get_count_nonneg(t, d, g);
        var s2 := ApplyRow(s0, r, 1);
        assert s2.count >= 1;
        assert CellKey(d, g) in Fold(t + [r]);
      } else {
        Get_ApplyDelta_other_cell(Fold(t), r, d, g);
        if CellKey(d, g) in Fold(t) {
          assert CellKey(d, g) in Fold(t + [r]);
        }
      }
    }
  }

  lemma {:isolate_assertions} AuxMerge_Fold_eq_FilteredContrib(
    t: seq<Row>, cutoff: int, order: seq<int>, g: int
  )
    requires RowsOk(t)
    requires OrderUnique(order)
    requires OrderCoversFoldDays(t, cutoff, g, order)
    ensures AuxMergeGroupAlongOrder(Fold(t), cutoff, order, g, 0, EmptyStats())
         == FilteredContrib(t, cutoff, g)
    decreases |t|
  {
    if |t| == 0 {
      AuxMerge_empty_store(cutoff, order, g, 0, EmptyStats());
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      OrderCovers_preserves_snoc(pref, r, cutoff, g, order);
      AuxMerge_Fold_eq_FilteredContrib(pref, cutoff, order, g);
      FoldStep(pref, r);
      FilteredContrib_step(pref, r, cutoff, g);
      Fold_Get_count_nonneg(pref, r.shipdate, RowKey(r));
      if r.shipdate <= cutoff && RowKey(r) == g {
        // Coverage on t ⇒ r.shipdate in order (cell present after insert)
        Fold_Get_count_nonneg(pref, r.shipdate, g);
        var s0 := Get(Fold(pref), CellKey(r.shipdate, g));
        var s2 := ApplyRow(s0, r, 1);
        assert s2.count >= 1;
        assert CellKey(r.shipdate, g) in Fold(t);
        assert r.shipdate in order;
        assert exists j :: 0 <= j < |order| && order[j] == r.shipdate;
        AuxMerge_ApplyDelta_contrib(Fold(pref), r, cutoff, order, g, 0, EmptyStats());
        Apply_eq_Merge_row(FilteredContrib(pref, cutoff, g), r);
        // IH: AuxMerge(Fold(pref)) == FilteredContrib(pref)
        // AuxMerge(Fold(t)) == Merge(AuxMerge(Fold(pref)), RowContrib)
        // FilteredContrib(t) == ApplyRow(FilteredContrib(pref), r, 1)
        //                   == Merge(FilteredContrib(pref), RowContrib)
      } else {
        AuxMerge_ApplyDelta_unchanged(Fold(pref), r, cutoff, order, g, 0, EmptyStats());
      }
    }
  }

  lemma TwinMerged_GetGroup_eq_FilteredContrib(
    days: Twin.DayStore, store: Store, db: seq<Row>, cutoff: int, g: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires PartsWellFormed(days)
    requires OrderUnique(days.order)
    requires OrderCoversFoldDays(db, cutoff, g, days.order)
    ensures StatsToAux(Twin.GetGroup(TwinMergedAtCutoff(days, cutoff), g))
         == FilteredContrib(db, cutoff, g)
  {
    TwinMerged_GetGroup_eq_TwinMergeAlong(days, cutoff, g);
    TwinMergeGroup_eq_AuxMergeGroup(days, cutoff, g, 0, F.EmptyStats());
    assert StatsToAux(F.EmptyStats()) == EmptyStats();
    assert Flatten(days) == store;
    assert store == Fold(db);
    AuxMerge_Fold_eq_FilteredContrib(db, cutoff, days.order, g);
  }

  lemma TwinMerged_GetGroup_eq_Compose(
    days: Twin.DayStore, store: Store, db: seq<Row>, cutoff: int, g: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires PartsWellFormed(days)
    requires OrderUnique(days.order)
    requires OrderCoversFoldDays(db, cutoff, g, days.order)
    ensures StatsToAux(Twin.GetGroup(TwinMergedAtCutoff(days, cutoff), g))
         == GetKey(Compose(db, cutoff), g)
  {
    TwinMerged_GetGroup_eq_FilteredContrib(days, store, db, cutoff, g);
    Compose_GetKey_eq_FilteredContrib(db, cutoff, g);
  }


  // =====================================================================
  // HB3b map package — DayPartToKeyStorePos == Compose
  // =====================================================================

  ghost function DayPartToKeyStorePos(part: Twin.DayPart): KeyStore {
    map g | g in part.groups && part.groups[g].count > 0 :: StatsToAux(part.groups[g])
  }

  // Cell-level agreement for all g.
  ghost predicate TwinMergedCellsEqCompose(
    days: Twin.DayStore, db: seq<Row>, cutoff: int
  )
    requires RowsOk(db)
  {
    forall g :: StatsToAux(Twin.GetGroup(TwinMergedAtCutoff(days, cutoff), g))
             == GetKey(Compose(db, cutoff), g)
  }

  lemma TwinMergedCells_to_KeyStorePos(
    days: Twin.DayStore, db: seq<Row>, cutoff: int
  )
    requires RowsOk(db)
    requires TwinMergedCellsEqCompose(days, db, cutoff)
    ensures DayPartToKeyStorePos(TwinMergedAtCutoff(days, cutoff)) == Compose(db, cutoff)
  {
    Bridge.Compose_positive_counts(db, cutoff);
    var merged := TwinMergedAtCutoff(days, cutoff);
    var m := DayPartToKeyStorePos(merged);
    var c := Compose(db, cutoff);
    assert forall g :: g in m <==> g in c by {
      forall g
        ensures (g in m) <==> (g in c)
      {
        assert StatsToAux(Twin.GetGroup(merged, g)) == GetKey(c, g);
        if g in c {
          assert c[g].count > 0;
          assert Twin.GetGroup(merged, g).count > 0;
          assert g in merged.groups;
          assert g in m;
        } else if g in m {
          assert Twin.GetGroup(merged, g).count > 0;
          assert GetKey(c, g).count > 0;
          assert g in c;
        }
      }
    }
    assert forall g :: g in m ==> m[g] == c[g] by {
      forall g | g in m
        ensures m[g] == c[g]
      {
        assert m[g] == StatsToAux(merged.groups[g]);
        assert StatsToAux(Twin.GetGroup(merged, g)) == GetKey(c, g);
        assert Twin.GetGroup(merged, g) == merged.groups[g];
        assert GetKey(c, g) == c[g];
      }
    }
  }

  // WellFormedDayStore + TwinAuxRel + Inv ⇒ every Fold cell day is in order.
  lemma OrderCovers_from_WellFormed(
    days: Twin.DayStore, store: Store, db: seq<Row>, cutoff: int, g: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    ensures OrderCoversFoldDays(db, cutoff, g, days.order)
  {
    forall d | CellKey(d, g) in Fold(db) && d <= cutoff
      ensures d in days.order
    {
      assert store == Fold(db);
      assert Flatten(days) == store;
      assert CellKey(d, g) in Flatten(days);
      assert d in days.parts && g in days.parts[d].groups;
      assert d in days.order;
    }
  }

  lemma TwinMerged_KeyStorePos_eq_Compose(
    days: Twin.DayStore, store: Store, db: seq<Row>, cutoff: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires PartsWellFormed(days)
    requires OrderUnique(days.order)
    ensures TwinMergedCellsEqCompose(days, db, cutoff)
    ensures DayPartToKeyStorePos(TwinMergedAtCutoff(days, cutoff)) == Compose(db, cutoff)
  {
    forall g
      ensures StatsToAux(Twin.GetGroup(TwinMergedAtCutoff(days, cutoff), g))
           == GetKey(Compose(db, cutoff), g)
    {
      OrderCovers_from_WellFormed(days, store, db, cutoff, g);
      TwinMerged_GetGroup_eq_Compose(days, store, db, cutoff, g);
    }
    TwinMergedCells_to_KeyStorePos(days, db, cutoff);
  }

  // =====================================================================
  // HB3b finalize — prove FinalizeSortFaithful via SameMapSameSortedSeq
  // =====================================================================

  ghost predicate FinalizeSortFaithful(part: Twin.DayPart, m: KeyStore)
  {
    F.FinalizeFromEntries(Twin.MapToEntries(part))
 == Bridge.AuxFinalizeKeys(Bridge.AuxKeysFrom(0, 65536, m), m)
  }

  ghost function OutRowsToEntries(rows: seq<F.OutRow>): seq<SU.Entry<F.OutRow>>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SU.Entry(rows[0].group_key, rows[0])] + OutRowsToEntries(rows[1..])
  }

  lemma OutRowsToEntries_len(rows: seq<F.OutRow>)
    ensures |OutRowsToEntries(rows)| == |rows|
    ensures forall i :: 0 <= i < |rows| ==>
              OutRowsToEntries(rows)[i].key == rows[i].group_key &&
              OutRowsToEntries(rows)[i].val == rows[i]
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      OutRowsToEntries_len(rows[1..]);
    }
  }

  lemma OutRowsToEntries_injective(a: seq<F.OutRow>, b: seq<F.OutRow>)
    requires OutRowsToEntries(a) == OutRowsToEntries(b)
    ensures a == b
  {
    OutRowsToEntries_len(a);
    OutRowsToEntries_len(b);
    if |a| == 0 {
      assert |b| == 0;
    } else {
      assert |b| == |a|;
      assert OutRowsToEntries(a)[0] == OutRowsToEntries(b)[0];
      assert OutRowsToEntries(a)[0].val == a[0];
      assert OutRowsToEntries(b)[0].val == b[0];
      assert a[0] == b[0];
      assert OutRowsToEntries(a)[1..] == OutRowsToEntries(a[1..]);
      assert OutRowsToEntries(b)[1..] == OutRowsToEntries(b[1..]);
      assert OutRowsToEntries(a[1..]) == OutRowsToEntries(b[1..]);
      OutRowsToEntries_injective(a[1..], b[1..]);
    }
  }

  lemma OutRowsToEntries_HasKey(rows: seq<F.OutRow>, k: int)
    ensures SU.HasKey(OutRowsToEntries(rows), k) <==>
            (exists i :: 0 <= i < |rows| && rows[i].group_key == k)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      OutRowsToEntries_len(rows);
      OutRowsToEntries_HasKey(rows[1..], k);
      assert OutRowsToEntries(rows)
          == [SU.Entry(rows[0].group_key, rows[0])] + OutRowsToEntries(rows[1..]);
      if rows[0].group_key == k {
        assert OutRowsToEntries(rows)[0].key == k;
        assert SU.HasKey(OutRowsToEntries(rows), k);
        assert exists i :: 0 <= i < |rows| && rows[i].group_key == k;
      } else {
        assert SU.HasKey(OutRowsToEntries(rows), k) <==>
               SU.HasKey(OutRowsToEntries(rows[1..]), k);
      }
    }
  }

  ghost predicate SortedByGroupKey(rows: seq<F.OutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key <= rows[i + 1].group_key
  }

  ghost predicate UniqueOutKeys(rows: seq<F.OutRow>) {
    forall i, j :: 0 <= i < j < |rows| ==> rows[i].group_key != rows[j].group_key
  }

  lemma InsertSorted_sorted(rows: seq<F.OutRow>, row: F.OutRow)
    requires SortedByGroupKey(rows)
    ensures SortedByGroupKey(F.InsertSorted(rows, row))
    decreases |rows|
  {
    if |rows| == 0 {
    } else if row.group_key < rows[0].group_key {
    } else {
      InsertSorted_sorted(rows[1..], row);
    }
  }

  lemma SortByKey_sorted(rows: seq<F.OutRow>)
    ensures SortedByGroupKey(F.SortByKey(rows))
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SortByKey_sorted(rows[1..]);
      InsertSorted_sorted(F.SortByKey(rows[1..]), rows[0]);
    }
  }

  lemma InsertSorted_preserves_unique(rows: seq<F.OutRow>, row: F.OutRow)
    requires UniqueOutKeys(rows)
    requires forall i :: 0 <= i < |rows| ==> rows[i].group_key != row.group_key
    ensures UniqueOutKeys(F.InsertSorted(rows, row))
    decreases |rows|
  {
    if |rows| == 0 {
    } else if row.group_key < rows[0].group_key {
      var out := [row] + rows;
      assert F.InsertSorted(rows, row) == out;
      forall i, j | 0 <= i < j < |out|
        ensures out[i].group_key != out[j].group_key
      {
        if i == 0 {
          assert out[j] == rows[j - 1];
          assert rows[j - 1].group_key != row.group_key;
        } else {
          assert out[i] == rows[i - 1];
          assert out[j] == rows[j - 1];
          assert rows[i - 1].group_key != rows[j - 1].group_key;
        }
      }
    } else {
      assert UniqueOutKeys(rows[1..]);
      forall i | 0 <= i < |rows[1..]|
        ensures rows[1..][i].group_key != row.group_key
      {
        assert rows[1..][i] == rows[i + 1];
      }
      InsertSorted_preserves_unique(rows[1..], row);
      var rest := F.InsertSorted(rows[1..], row);
      var out := [rows[0]] + rest;
      assert F.InsertSorted(rows, row) == out;
      forall i, j | 0 <= i < j < |out|
        ensures out[i].group_key != out[j].group_key
      {
        if i == 0 {
          assert out[0] == rows[0];
          assert out[j] == rest[j - 1];
          InsertSorted_elem_from(rows[1..], row, j - 1);
          if rest[j - 1] == row {
            assert rows[0].group_key != row.group_key;
          } else {
            var t :| 0 <= t < |rows[1..]| && rest[j - 1] == rows[1..][t];
            assert rows[0].group_key != rows[1..][t].group_key;
          }
        } else {
          assert out[i] == rest[i - 1];
          assert out[j] == rest[j - 1];
          assert rest[i - 1].group_key != rest[j - 1].group_key;
        }
      }
    }
  }

  lemma SortByKey_preserves_unique(rows: seq<F.OutRow>)
    requires UniqueOutKeys(rows)
    ensures UniqueOutKeys(F.SortByKey(rows))
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      assert UniqueOutKeys(rows[1..]);
      forall i | 0 <= i < |rows[1..]|
        ensures rows[1..][i].group_key != rows[0].group_key
      {
        assert rows[0].group_key != rows[i + 1].group_key;
      }
      SortByKey_preserves_unique(rows[1..]);
      var st := F.SortByKey(rows[1..]);
      forall i | 0 <= i < |st|
        ensures st[i].group_key != rows[0].group_key
      {
        SortByKey_elem_from(rows[1..], i);
        var j :| 0 <= j < |rows[1..]| && rows[1..][j] == st[i];
        assert st[i].group_key == rows[1..][j].group_key;
        assert rows[1..][j].group_key != rows[0].group_key;
      }
      InsertSorted_preserves_unique(st, rows[0]);
    }
  }

  lemma SortedUnique_Out_implies_Strict(rows: seq<F.OutRow>)
    requires SortedByGroupKey(rows)
    requires UniqueOutKeys(rows)
    ensures SU.StrictSorted(OutRowsToEntries(rows))
  {
    forall i | 0 <= i < |rows| - 1
      ensures rows[i].group_key < rows[i + 1].group_key
    {
      assert rows[i].group_key <= rows[i + 1].group_key;
      assert rows[i].group_key != rows[i + 1].group_key;
    }
  }

  lemma MapToEntriesFrom_unique(part: Twin.DayPart, j: nat)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    ensures forall i1, i2 ::
              0 <= i1 < i2 < |Twin.MapToEntriesFrom(part, j)| ==>
              Twin.MapToEntriesFrom(part, j)[i1].0 != Twin.MapToEntriesFrom(part, j)[i2].0
    decreases |part.keys| - j
  {
    if j == |part.keys| {
    } else if part.keys[j] !in part.groups {
      assert false;
    } else {
      MapToEntriesFrom_unique(part, j + 1);
      var rest := Twin.MapToEntriesFrom(part, j + 1);
      forall i1, i2 | 0 <= i1 < i2 < |Twin.MapToEntriesFrom(part, j)|
        ensures Twin.MapToEntriesFrom(part, j)[i1].0 != Twin.MapToEntriesFrom(part, j)[i2].0
      {
        if i1 == 0 {
          assert Twin.MapToEntriesFrom(part, j)[0].0 == part.keys[j];
          assert Twin.MapToEntriesFrom(part, j)[i2].0 == rest[i2 - 1].0;
          MapToEntriesFrom_key_from(part, j + 1, i2 - 1);
          var t :| j + 1 <= t < |part.keys| &&
                   part.keys[t] == Twin.MapToEntriesFrom(part, j)[i2].0;
          assert part.keys[j] != part.keys[t];
        }
      }
    }
  }

  lemma MapToEntriesFrom_key_from(part: Twin.DayPart, j: nat, i: nat)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    requires i < |Twin.MapToEntriesFrom(part, j)|
    ensures exists t :: j <= t < |part.keys| &&
              part.keys[t] == Twin.MapToEntriesFrom(part, j)[i].0
    decreases |part.keys| - j
  {
    if j == |part.keys| {
      assert false;
    } else if part.keys[j] !in part.groups {
      assert false;
    } else if i == 0 {
    } else {
      MapToEntriesFrom_key_from(part, j + 1, i - 1);
    }
  }

  lemma MapToEntries_unique(part: Twin.DayPart)
    requires WellFormedPart(part)
    ensures forall i1, i2 ::
              0 <= i1 < i2 < |Twin.MapToEntries(part)| ==>
              Twin.MapToEntries(part)[i1].0 != Twin.MapToEntries(part)[i2].0
  {
    MapToEntriesFrom_unique(part, 0);
  }

  lemma CollectFinalize_keys_from(entries: seq<(int, F.Stats)>, i: nat)
    requires i < |F.CollectFinalize(entries)|
    ensures exists t :: 0 <= t < |entries| &&
              entries[t].0 == F.CollectFinalize(entries)[i].group_key &&
              entries[t].1.count > 0
    decreases |entries|
  {
    if |entries| == 0 {
      assert false;
    } else {
      var head := F.FinalizeOneEntry(entries[0].0, entries[0].1);
      if |head| == 0 {
        CollectFinalize_keys_from(entries[1..], i);
      } else if i == 0 {
        assert entries[0].1.count > 0;
      } else {
        CollectFinalize_keys_from(entries[1..], i - 1);
      }
    }
  }

  lemma CollectFinalize_unique(entries: seq<(int, F.Stats)>)
    requires forall i1, i2 :: 0 <= i1 < i2 < |entries| ==> entries[i1].0 != entries[i2].0
    ensures UniqueOutKeys(F.CollectFinalize(entries))
    decreases |entries|
  {
    if |entries| == 0 {
    } else {
      CollectFinalize_unique(entries[1..]);
      var head := F.FinalizeOneEntry(entries[0].0, entries[0].1);
      var rest := F.CollectFinalize(entries[1..]);
      if |head| == 0 {
      } else {
        assert |head| == 1 && head[0].group_key == entries[0].0;
        forall i | 0 <= i < |rest|
          ensures head[0].group_key != rest[i].group_key
        {
          CollectFinalize_keys_from(entries[1..], i);
        }
      }
    }
  }

  lemma MapToEntries_has(part: Twin.DayPart, g: int)
    requires WellFormedPart(part)
    requires g in part.groups
    ensures exists i :: 0 <= i < |Twin.MapToEntries(part)| &&
              Twin.MapToEntries(part)[i] == (g, part.groups[g])
  {
    assert exists i0 :: 0 <= i0 < |part.keys| && part.keys[i0] == g;
    MapToEntriesFrom_has(part, 0, g);
    assert Twin.MapToEntries(part) == Twin.MapToEntriesFrom(part, 0);
  }

  lemma MapToEntriesFrom_has(part: Twin.DayPart, j: nat, g: int)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    requires g in part.groups
    requires exists i :: j <= i < |part.keys| && part.keys[i] == g
    ensures exists i :: 0 <= i < |Twin.MapToEntriesFrom(part, j)| &&
              Twin.MapToEntriesFrom(part, j)[i] == (g, part.groups[g])
    decreases |part.keys| - j
  {
    if j == |part.keys| {
      assert false;
    } else if part.keys[j] !in part.groups {
      assert false;
    } else if part.keys[j] == g {
      assert Twin.MapToEntriesFrom(part, j)[0] == (g, part.groups[g]);
    } else {
      var iw :| j <= iw < |part.keys| && part.keys[iw] == g;
      assert iw != j;
      assert j + 1 <= iw < |part.keys|;
      MapToEntriesFrom_has(part, j + 1, g);
      var rest := Twin.MapToEntriesFrom(part, j + 1);
      var i :| 0 <= i < |rest| && rest[i] == (g, part.groups[g]);
      assert Twin.MapToEntriesFrom(part, j)[i + 1] == (g, part.groups[g]);
    }
  }

  lemma CollectFinalize_has(entries: seq<(int, F.Stats)>, g: int, s: F.Stats)
    requires exists i :: 0 <= i < |entries| && entries[i] == (g, s) && s.count > 0
    requires forall i1, i2 :: 0 <= i1 < i2 < |entries| ==> entries[i1].0 != entries[i2].0
    ensures exists i :: 0 <= i < |F.CollectFinalize(entries)| &&
              F.CollectFinalize(entries)[i] == F.FinalizeOneEntry(g, s)[0]
    decreases |entries|
  {
    if |entries| == 0 {
      assert false;
    } else if entries[0] == (g, s) {
      assert F.CollectFinalize(entries)[0] == F.FinalizeOneEntry(g, s)[0];
    } else {
      CollectFinalize_has(entries[1..], g, s);
      var head := F.FinalizeOneEntry(entries[0].0, entries[0].1);
      var rest := F.CollectFinalize(entries[1..]);
      var i :| 0 <= i < |rest| && rest[i] == F.FinalizeOneEntry(g, s)[0];
      assert F.CollectFinalize(entries)[|head| + i] == F.FinalizeOneEntry(g, s)[0];
    }
  }

  lemma InsertSorted_has(rows: seq<F.OutRow>, row: F.OutRow)
    ensures exists i :: 0 <= i < |F.InsertSorted(rows, row)| &&
              F.InsertSorted(rows, row)[i] == row
    decreases |rows|
  {
    if |rows| == 0 {
      assert F.InsertSorted(rows, row)[0] == row;
    } else if row.group_key < rows[0].group_key {
      assert F.InsertSorted(rows, row)[0] == row;
    } else {
      InsertSorted_has(rows[1..], row);
      var i :| 0 <= i < |F.InsertSorted(rows[1..], row)| &&
                F.InsertSorted(rows[1..], row)[i] == row;
      assert F.InsertSorted(rows, row)[i + 1] == row;
    }
  }

  lemma InsertSorted_keeps(rows: seq<F.OutRow>, head: F.OutRow, row: F.OutRow)
    requires exists i :: 0 <= i < |rows| && rows[i] == row
    ensures exists i :: 0 <= i < |F.InsertSorted(rows, head)| &&
              F.InsertSorted(rows, head)[i] == row
    decreases |rows|
  {
    var i0 :| 0 <= i0 < |rows| && rows[i0] == row;
    if |rows| == 0 {
      assert false;
    } else if head.group_key < rows[0].group_key {
      assert F.InsertSorted(rows, head)[i0 + 1] == row;
    } else if i0 == 0 {
      assert F.InsertSorted(rows, head)[0] == rows[0] == row;
    } else {
      assert exists i :: 0 <= i < |rows[1..]| && rows[1..][i] == row;
      InsertSorted_keeps(rows[1..], head, row);
      var i :| 0 <= i < |F.InsertSorted(rows[1..], head)| &&
                F.InsertSorted(rows[1..], head)[i] == row;
      assert F.InsertSorted(rows, head)[i + 1] == row;
    }
  }

  lemma SortByKey_has(rows: seq<F.OutRow>, row: F.OutRow)
    requires exists i :: 0 <= i < |rows| && rows[i] == row
    ensures exists i :: 0 <= i < |F.SortByKey(rows)| && F.SortByKey(rows)[i] == row
    decreases |rows|
  {
    var i0 :| 0 <= i0 < |rows| && rows[i0] == row;
    if |rows| == 0 {
      assert false;
    } else if i0 == 0 {
      assert row == rows[0];
      assert F.SortByKey(rows)
          == F.InsertSorted(F.SortByKey(rows[1..]), rows[0]);
      InsertSorted_has(F.SortByKey(rows[1..]), rows[0]);
      var i :| 0 <= i < |F.InsertSorted(F.SortByKey(rows[1..]), rows[0])| &&
                F.InsertSorted(F.SortByKey(rows[1..]), rows[0])[i] == rows[0];
      assert F.SortByKey(rows)[i] == row;
    } else {
      assert rows[1..][i0 - 1] == row;
      SortByKey_has(rows[1..], row);
      assert exists i :: 0 <= i < |F.SortByKey(rows[1..])| &&
                F.SortByKey(rows[1..])[i] == row;
      InsertSorted_keeps(F.SortByKey(rows[1..]), rows[0], row);
      var i :| 0 <= i < |F.InsertSorted(F.SortByKey(rows[1..]), rows[0])| &&
                F.InsertSorted(F.SortByKey(rows[1..]), rows[0])[i] == row;
      assert F.SortByKey(rows)
          == F.InsertSorted(F.SortByKey(rows[1..]), rows[0]);
      assert F.SortByKey(rows)[i] == row;
    }
  }

  lemma AuxFinalizeKeys_strict_sorted(keys: seq<int>, m: KeyStore)
    requires forall i :: 0 <= i < |keys| - 1 ==> keys[i] < keys[i + 1]
    requires forall i :: 0 <= i < |keys| ==> keys[i] in m && m[keys[i]].count > 0
    ensures forall i :: 0 <= i < |Bridge.AuxFinalizeKeys(keys, m)| - 1 ==>
              (Bridge.AuxFinalizeKeys(keys, m)[i].group_key
               < Bridge.AuxFinalizeKeys(keys, m)[i + 1].group_key)
    ensures |Bridge.AuxFinalizeKeys(keys, m)| == |keys|
    ensures forall i :: 0 <= i < |keys| ==>
              (Bridge.AuxFinalizeKeys(keys, m)[i]
               == Bridge.ExecGroup(keys[i], m[keys[i]]))
    decreases |keys|
  {
    if |keys| == 0 {
    } else {
      assert keys[0] in m && m[keys[0]].count > 0;
      AuxFinalizeKeys_strict_sorted(keys[1..], m);
      Bridge.AuxFinalizeKeys_len(keys, m);
    }
  }

  lemma InsertSorted_elem_from(rows: seq<F.OutRow>, head: F.OutRow, i: nat)
    requires i < |F.InsertSorted(rows, head)|
    ensures F.InsertSorted(rows, head)[i] == head ||
            (exists j :: 0 <= j < |rows| && F.InsertSorted(rows, head)[i] == rows[j])
    decreases |rows|
  {
    if |rows| == 0 {
      assert F.InsertSorted(rows, head)[0] == head;
    } else if head.group_key < rows[0].group_key {
      if i == 0 {
        assert F.InsertSorted(rows, head)[0] == head;
      } else {
        assert F.InsertSorted(rows, head)[i] == rows[i - 1];
      }
    } else {
      if i == 0 {
        assert F.InsertSorted(rows, head)[0] == rows[0];
      } else {
        InsertSorted_elem_from(rows[1..], head, i - 1);
      }
    }
  }

  lemma SortByKey_elem_from(rows: seq<F.OutRow>, i: nat)
    requires i < |F.SortByKey(rows)|
    ensures exists j :: 0 <= j < |rows| && rows[j] == F.SortByKey(rows)[i]
    decreases |rows|
  {
    if |rows| == 0 {
      assert false;
    } else {
      var st := F.SortByKey(rows[1..]);
      InsertSorted_elem_from(st, rows[0], i);
      if F.InsertSorted(st, rows[0])[i] == rows[0] {
        assert rows[0] == F.SortByKey(rows)[i];
      } else {
        var j :| 0 <= j < |st| && st[j] == F.InsertSorted(st, rows[0])[i];
        SortByKey_elem_from(rows[1..], j);
      }
    }
  }

  lemma MapToEntries_stats(part: Twin.DayPart, i: nat)
    requires WellFormedPart(part)
    requires i < |Twin.MapToEntries(part)|
    ensures Twin.MapToEntries(part)[i].0 in part.groups
    ensures Twin.MapToEntries(part)[i].1
         == part.groups[Twin.MapToEntries(part)[i].0]
  {
    MapToEntriesFrom_stats(part, 0, i);
  }

  lemma MapToEntriesFrom_stats(part: Twin.DayPart, j: nat, i: nat)
    requires WellFormedPart(part)
    requires j <= |part.keys|
    requires i < |Twin.MapToEntriesFrom(part, j)|
    ensures Twin.MapToEntriesFrom(part, j)[i].0 in part.groups
    ensures Twin.MapToEntriesFrom(part, j)[i].1
         == part.groups[Twin.MapToEntriesFrom(part, j)[i].0]
    decreases |part.keys| - j
  {
    if j == |part.keys| {
      assert false;
    } else if part.keys[j] !in part.groups {
      assert false;
    } else if i == 0 {
    } else {
      MapToEntriesFrom_stats(part, j + 1, i - 1);
    }
  }

  lemma TwinOut_key_in_map(part: Twin.DayPart, m: KeyStore, i: nat)
    requires WellFormedPart(part)
    requires DayPartToKeyStorePos(part) == m
    requires i < |F.FinalizeFromEntries(Twin.MapToEntries(part))|
    ensures F.FinalizeFromEntries(Twin.MapToEntries(part))[i].group_key in m
  {
    var entries := Twin.MapToEntries(part);
    var collected := F.CollectFinalize(entries);
    var twin_out := F.SortByKey(collected);
    assert twin_out == F.FinalizeFromEntries(entries);
    SortByKey_elem_from(collected, i);
    var j :| 0 <= j < |collected| && collected[j] == twin_out[i];
    CollectFinalize_keys_from(entries, j);
    var t :| 0 <= t < |entries| &&
             entries[t].0 == collected[j].group_key &&
             entries[t].1.count > 0;
    MapToEntries_stats(part, t);
    assert entries[t].1 == part.groups[entries[t].0];
    assert part.groups[entries[t].0].count > 0;
    assert entries[t].0 in DayPartToKeyStorePos(part);
  }

  lemma TwinOut_row_eq_FinalizeOne(
    part: Twin.DayPart, twin_out: seq<F.OutRow>, i: nat
  )
    requires WellFormedPart(part)
    requires twin_out == F.FinalizeFromEntries(Twin.MapToEntries(part))
    requires UniqueOutKeys(twin_out)
    requires i < |twin_out|
    requires twin_out[i].group_key in part.groups
    requires part.groups[twin_out[i].group_key].count > 0
    ensures twin_out[i]
         == F.FinalizeOneEntry(twin_out[i].group_key,
                               part.groups[twin_out[i].group_key])[0]
  {
    var g := twin_out[i].group_key;
    MapToEntries_unique(part);
    MapToEntries_has(part, g);
    CollectFinalize_has(Twin.MapToEntries(part), g, part.groups[g]);
    var collected := F.CollectFinalize(Twin.MapToEntries(part));
    var ci :| 0 <= ci < |collected| &&
              collected[ci] == F.FinalizeOneEntry(g, part.groups[g])[0];
    SortByKey_has(collected, F.FinalizeOneEntry(g, part.groups[g])[0]);
    var oi :| 0 <= oi < |twin_out| &&
              twin_out[oi] == F.FinalizeOneEntry(g, part.groups[g])[0];
    assert oi == i;
  }

  lemma AuxOut_row_at(keys: seq<int>, m: KeyStore, j: nat)
    requires forall i :: 0 <= i < |keys| ==> keys[i] in m && m[keys[i]].count > 0
    requires forall i :: 0 <= i < |keys| - 1 ==> keys[i] < keys[i + 1]
    requires j < |keys|
    ensures |Bridge.AuxFinalizeKeys(keys, m)| == |keys|
    ensures Bridge.AuxFinalizeKeys(keys, m)[j]
         == Bridge.ExecGroup(keys[j], m[keys[j]])
    ensures Bridge.AuxFinalizeKeys(keys, m)[j].group_key == keys[j]
    ensures keys[j] in m
  {
    Bridge.AuxFinalizeKeys_len(keys, m);
  }

  lemma {:isolate_assertions} FinalizeSortFaithful_from_map(
    part: Twin.DayPart, m: KeyStore
  )
    requires WellFormedPart(part)
    requires DayPartToKeyStorePos(part) == m
    requires forall g :: g in m ==> 0 <= g < 65536 && m[g].count > 0
    ensures FinalizeSortFaithful(part, m)
  {
    MapToEntries_unique(part);
    var entries := Twin.MapToEntries(part);
    CollectFinalize_unique(entries);
    var collected := F.CollectFinalize(entries);
    SortByKey_sorted(collected);
    SortByKey_preserves_unique(collected);
    var twin_out := F.SortByKey(collected);
    assert F.FinalizeFromEntries(entries) == twin_out;
    SortedUnique_Out_implies_Strict(twin_out);

    var keys := Bridge.AuxKeysFrom(0, 65536, m);
    Bridge.AuxKeysFrom_strict_sorted(0, 65536, m);
    Bridge.AuxKeysFrom_Finalize_pre(0, 65536, m);
    AuxFinalizeKeys_strict_sorted(keys, m);
    var aux_out := Bridge.AuxFinalizeKeys(keys, m);

    var te := OutRowsToEntries(twin_out);
    var ae := OutRowsToEntries(aux_out);
    OutRowsToEntries_len(twin_out);
    OutRowsToEntries_len(aux_out);
    assert SU.StrictSorted(te);
    forall i | 0 <= i < |ae| - 1
      ensures ae[i].key < ae[i + 1].key
    {
      assert ae[i].key == aux_out[i].group_key;
      assert ae[i + 1].key == aux_out[i + 1].group_key;
    }
    assert SU.StrictSorted(ae);

    forall k
      ensures SU.HasKey(te, k) <==> SU.HasKey(ae, k)
    {
      OutRowsToEntries_HasKey(twin_out, k);
      OutRowsToEntries_HasKey(aux_out, k);
      if k in m {
        assert k in part.groups && part.groups[k].count > 0;
        MapToEntries_has(part, k);
        CollectFinalize_has(entries, k, part.groups[k]);
        var ci :| 0 <= ci < |collected| &&
                  collected[ci] == F.FinalizeOneEntry(k, part.groups[k])[0];
        SortByKey_has(collected, F.FinalizeOneEntry(k, part.groups[k])[0]);
        assert exists i :: 0 <= i < |twin_out| && twin_out[i].group_key == k;
        Bridge.AuxKeysFrom_contains_fwd(0, 65536, m, k);
        var ki :| 0 <= ki < |keys| && keys[ki] == k;
        assert aux_out[ki].group_key == k;
      } else {
        forall i | 0 <= i < |twin_out|
          ensures twin_out[i].group_key != k
        {
          TwinOut_key_in_map(part, m, i);
        }
        forall i | 0 <= i < |aux_out|
          ensures aux_out[i].group_key != k
        {
          AuxOut_row_at(keys, m, i);
        }
      }
    }

    forall i, j | 0 <= i < |te| && 0 <= j < |ae| && te[i].key == ae[j].key
      ensures te[i].val == ae[j].val
    {
      var g := te[i].key;
      assert i < |twin_out| && j < |aux_out|;
      assert twin_out[i].group_key == g;
      assert aux_out[j].group_key == g;
      TwinOut_key_in_map(part, m, i);
      assert g in m;
      assert g in part.groups && part.groups[g].count > 0;
      TwinOut_row_eq_FinalizeOne(part, twin_out, i);
      AuxOut_row_at(keys, m, j);
      assert keys[j] == g;
      assert aux_out[j] == Bridge.ExecGroup(g, m[g]);
      assert StatsToAux(part.groups[g]) == m[g];
      StatsRoundTrip(part.groups[g]);
      assert Bridge.ExecGroup(g, m[g])
          == F.FinalizeOneEntry(g, part.groups[g])[0];
      assert twin_out[i] == aux_out[j];
      assert te[i].val == twin_out[i];
      assert ae[j].val == aux_out[j];
    }

    SU.SameMapSameSortedSeq(te, ae);
    OutRowsToEntries_injective(twin_out, aux_out);
  }

  lemma MergedPart_WellFormed(days: Twin.DayStore, cutoff: int)
    requires PartsWellFormed(days)
    ensures WellFormedPart(TwinMergedAtCutoff(days, cutoff))
  {
    EmptyPart_WellFormed();
    ComposeMergeDays_GetGroup(days, cutoff, 0, Twin.EmptyPart(), 0);
  }

  lemma ComposeOutput_eq_ExecSeqM(
    days: Twin.DayStore, db: seq<Row>, cutoff: int
  )
    requires RowsOk(db)
    requires TwinMergedCellsEqCompose(days, db, cutoff)
    requires WellFormedPart(TwinMergedAtCutoff(days, cutoff))
    requires forall g :: g in Compose(db, cutoff) ==> 0 <= g < 65536
    requires FinalizeSortFaithful(
               TwinMergedAtCutoff(days, cutoff), Compose(db, cutoff))
    ensures Twin.ComposeOutput(days, cutoff) == Bridge.ExecSeqM(db, cutoff)
  {
    TwinMergedCells_to_KeyStorePos(days, db, cutoff);
    Bridge.Compose_positive_counts(db, cutoff);
    assert Twin.ComposeOutput(days, cutoff)
        == F.FinalizeFromEntries(Twin.MapToEntries(TwinMergedAtCutoff(days, cutoff)));
    assert Bridge.ExecSeqM(db, cutoff)
        == Bridge.AuxFinalizeKeys(
             Bridge.AuxKeysFrom(0, 65536, Compose(db, cutoff)),
             Compose(db, cutoff));
  }

  lemma TwinExecRefinesAux_ComposeEqAux(
    days: Twin.DayStore, store: Store, db: seq<Row>, cutoff: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires PartsWellFormed(days)
    requires OrderUnique(days.order)
    ensures Twin.ComposeOutput(days, cutoff) == Bridge.ExecSeqM(db, cutoff)
  {
    TwinMerged_KeyStorePos_eq_Compose(days, store, db, cutoff);
    MergedPart_WellFormed(days, cutoff);
    Bridge.Compose_keys_ok(db, cutoff);
    var merged := TwinMergedAtCutoff(days, cutoff);
    var m := Compose(db, cutoff);
    assert DayPartToKeyStorePos(merged) == m;
    FinalizeSortFaithful_from_map(merged, m);
    ComposeOutput_eq_ExecSeqM(days, db, cutoff);
  }

  lemma TwinExecRefinesAux_hit_full(
    args: Twin.QueryArgs,
    base_days: int,
    days: Twin.DayStore,
    store: Store,
    db: seq<Row>,
    built: Twin.DayStore
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires PartsWellFormed(days)
    requires OrderUnique(days.order)
    requires args.has_offset
    ensures Twin.Execute(args, base_days, true, true, days, built)
         == Bridge.ExecSeqM(db, Twin.TpchQ1CutoffDays(base_days, args.day_offset))
  {
    var cutoff := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
    TwinExec_hit_eq_ComposeOutput(args, base_days, days, built);
    TwinExecRefinesAux_ComposeEqAux(days, store, db, cutoff);
  }

  // =====================================================================
  // HB3c — fallback under BuiltInvAtCutoff (instantiated from producers)
  // =====================================================================

  // Store bookkeeping + flatten oracle. FinalizeSortFaithful is derived, not assumed.
  ghost predicate BuiltInvAtCutoff(built: Twin.DayStore, db: seq<Row>, cutoff: int)
    requires RowsOk(db)
  {
    TwinStoreOk(built) &&
    RebuildOracleCorrect(built, FoldTwin(db))
  }

  lemma BuiltInvAtCutoff_implies_FinalizeSortFaithful(
    built: Twin.DayStore, db: seq<Row>, cutoff: int
  )
    requires RowsOk(db)
    requires BuiltInvAtCutoff(built, db, cutoff)
    ensures FinalizeSortFaithful(
              TwinMergedAtCutoff(built, cutoff), Compose(db, cutoff))
  {
    RebuildEstablishesInv(db);
    assert TwinAuxRel(built, FoldTwin(db));
    assert Inv(FoldTwin(db), db);
    assert TwinExecRefinesAux_Premises(built, FoldTwin(db), db);
    TwinMerged_KeyStorePos_eq_Compose(built, FoldTwin(db), db, cutoff);
    MergedPart_WellFormed(built, cutoff);
    Bridge.Compose_keys_ok(db, cutoff);
    FinalizeSortFaithful_from_map(
      TwinMergedAtCutoff(built, cutoff), Compose(db, cutoff));
  }

  lemma BuiltInvAtCutoff_from_TwinStore(
    built: Twin.DayStore, db: seq<Row>, cutoff: int
  )
    requires RowsOk(db)
    requires TwinStoreOk(built)
    requires RebuildOracleCorrect(built, FoldTwin(db))
    ensures BuiltInvAtCutoff(built, db, cutoff)
  {
  }

  lemma BuiltInvAtCutoff_from_BuildState_lineitems(
    inputs: seq<Twin.LineitemInput>, cutoff: int
  )
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures var built := Twin.BuildStateForDays({}, [], false, [], false, inputs);
            var db := InputsToRows(inputs);
            BuiltInvAtCutoff(built, db, cutoff)
  {
    InputsToRows_ok(inputs);
    BuildStateForDays_lineitems_RebuildOracleCorrect(inputs);
    BuildStateForDays_preserves_TwinStoreOk({}, [], false, [], false, inputs);
  }

  lemma BuiltInvAtCutoff_from_BuildState_aggs_for_days(
    aggs: seq<Twin.DayGroupAggregate>,
    days_filter: set<int>,
    table: seq<Row>,
    cutoff: int
  )
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    requires RowsOk(table)
    requires days_filter != {}
    requires AggsFaithful(aggs, days_filter, table)
    ensures var built := Twin.BuildStateForDays(
              days_filter, aggs, true, [], false, []);
            var db := FilterTableByDays(table, days_filter);
            BuiltInvAtCutoff(built, db, cutoff)
  {
    BuildState_aggs_for_days_RebuildOracleCorrect(aggs, days_filter, table);
    BuildStateForDays_preserves_TwinStoreOk(days_filter, aggs, true, [], false, []);
  }

  lemma BuiltInvAtCutoff_from_BuildState_aggs_all(
    aggs: seq<Twin.DayGroupAggregate>,
    days_filter: set<int>,
    table: seq<Row>,
    cutoff: int
  )
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    requires RowsOk(table)
    requires AggsFaithful(aggs, days_filter, table)
    ensures var built := Twin.BuildStateForDays(
              days_filter, [], false, aggs, true, []);
            var db := FilterTableByDays(table, days_filter);
            BuiltInvAtCutoff(built, db, cutoff)
  {
    BuildState_aggs_all_RebuildOracleCorrect(aggs, days_filter, table);
    BuildStateForDays_preserves_TwinStoreOk(days_filter, [], false, aggs, true, []);
  }

  // Load establishes TwinStoreOk/OrderExact; content oracle is still RebuildOracleCorrect
  // (decoded images are not a Fold derivation without that contract).
  lemma BuiltInvAtCutoff_from_LoadPartitions(
    cutoff: int,
    images: seq<Twin.PartImage>,
    db: seq<Row>
  )
    requires RowsOk(db)
    requires forall i :: 0 <= i < |images| && images[i].ok ==>
              WellFormedPart(images[i].part)
    requires forall i, j :: 0 <= i < j < |images| && images[i].ok && images[j].ok ==>
              images[i].shipdate_days != images[j].shipdate_days
    requires var r := Twin.LoadPartitionsTwin(cutoff, images);
             r.0 && RebuildOracleCorrect(r.1, FoldTwin(db))
    ensures var r := Twin.LoadPartitionsTwin(cutoff, images);
            BuiltInvAtCutoff(r.1, db, cutoff)
  {
    LoadPartitionsTwin_preserves_TwinStoreOk(cutoff, images);
  }

  lemma TwinExec_fallback_under_BuiltInv(
    args: Twin.QueryArgs,
    base_days: int,
    store_days: Twin.DayStore,
    built: Twin.DayStore,
    db: seq<Row>,
    valid: bool,
    load_ok: bool
  )
    requires RowsOk(db)
    requires args.has_offset
    requires var cutoff := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
             BuiltInvAtCutoff(built, db, cutoff)
    requires !valid || !load_ok
    ensures Twin.Execute(args, base_days, valid, load_ok, store_days, built)
         == Bridge.ExecSeqM(db, Twin.TpchQ1CutoffDays(base_days, args.day_offset))
  {
    var cutoff := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
    assert Twin.Execute(args, base_days, valid, load_ok, store_days, built)
        == Twin.ComposeOutput(built, cutoff);
    RebuildEstablishesInv(db);
    assert TwinAuxRel(built, FoldTwin(db));
    assert Inv(FoldTwin(db), db);
    BuiltInvAtCutoff_implies_FinalizeSortFaithful(built, db, cutoff);
    TwinExecRefinesAux_ComposeEqAux(built, FoldTwin(db), db, cutoff);
  }

  // =====================================================================
  // Closed Twin execution + maintenance (SPI residual: AggsFaithful only)
  // =====================================================================

  lemma Q1Twin_Execute_hit_closed(
    args: Twin.QueryArgs,
    base_days: int,
    days: Twin.DayStore,
    store: Store,
    db: seq<Row>,
    built: Twin.DayStore
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires TwinStoreOk(days)
    requires args.has_offset
    ensures Twin.Execute(args, base_days, true, true, days, built)
         == Bridge.ExecSeqM(db, Twin.TpchQ1CutoffDays(base_days, args.day_offset))
  {
    TwinExecRefinesAux_hit_full(args, base_days, days, store, db, built);
  }

  lemma Q1Twin_Execute_fallback_from_BuildState_lineitems_closed(
    args: Twin.QueryArgs,
    base_days: int,
    store_days: Twin.DayStore,
    inputs: seq<Twin.LineitemInput>,
    valid: bool,
    load_ok: bool
  )
    requires args.has_offset
    requires !valid || !load_ok
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures Twin.Execute(
              args, base_days, valid, load_ok, store_days,
              Twin.BuildStateForDays({}, [], false, [], false, inputs))
         == Bridge.ExecSeqM(
              InputsToRows(inputs),
              Twin.TpchQ1CutoffDays(base_days, args.day_offset))
  {
    var cutoff := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
    var built := Twin.BuildStateForDays({}, [], false, [], false, inputs);
    var db := InputsToRows(inputs);
    BuiltInvAtCutoff_from_BuildState_lineitems(inputs, cutoff);
    TwinExec_fallback_under_BuiltInv(
      args, base_days, store_days, built, db, valid, load_ok);
  }

  lemma Q1Twin_Maint_ApplyDeltaSeq_closed(
    twin: Twin.DayStore, store: Store, ds: seq<Twin.LineitemDelta>
  )
    requires TwinAuxRel(twin, store)
    requires TwinStoreOk(twin)
    requires OrderExact(twin)
    ensures TwinAuxRel(Twin.ApplyDeltaSeq(twin, ds), ApplyTwinDeltaSeq(store, ds))
    ensures TwinStoreOk(Twin.ApplyDeltaSeq(twin, ds))
    ensures OrderExact(Twin.ApplyDeltaSeq(twin, ds))
  {
    TwinMaintRefinesAux_ApplyDeltaSeq(twin, store, ds);
    ApplyDeltaSeq_preserves_TwinStoreOk(twin, ds);
  }

  lemma Q1Twin_Maint_DeleteSuffix_closed(
    twin: Twin.DayStore, s: AuxState, rows: seq<Row>
  )
    requires TwinAuxRel(twin, s.store)
    requires TwinStoreOk(twin)
    requires OrderExact(twin)
    requires StateInv(s)
    requires ActionOk(s, ApplyDeleteSuffix(rows))
    ensures TwinAuxRel(
              Twin.ApplyDeltaSeq(twin, RowsToDeleteDeltas(rows)),
              ApplyMaint(s, ApplyDeleteSuffix(rows)).store)
    ensures TwinStoreOk(Twin.ApplyDeltaSeq(twin, RowsToDeleteDeltas(rows)))
    ensures OrderExact(Twin.ApplyDeltaSeq(twin, RowsToDeleteDeltas(rows)))
  {
    TwinMaintRefinesAux_DeleteSuffix(twin, s, rows);
    ApplyDeltaSeq_preserves_TwinStoreOk(twin, RowsToDeleteDeltas(rows));
  }

  lemma Q1Twin_Maint_InsertSeq_closed(
    twin: Twin.DayStore, s: AuxState, rows: seq<Row>
  )
    requires TwinAuxRel(twin, s.store)
    requires TwinStoreOk(twin)
    requires OrderExact(twin)
    requires StateInv(s)
    requires ActionOk(s, ApplyInsertSeq(rows))
    ensures TwinAuxRel(
              Twin.ApplyDeltaSeq(twin, RowsToInsertDeltas(rows)),
              ApplyMaint(s, ApplyInsertSeq(rows)).store)
    ensures TwinStoreOk(Twin.ApplyDeltaSeq(twin, RowsToInsertDeltas(rows)))
    ensures OrderExact(Twin.ApplyDeltaSeq(twin, RowsToInsertDeltas(rows)))
  {
    TwinMaintRefinesAux_InsertSeq(twin, s, rows);
    ApplyDeltaSeq_preserves_TwinStoreOk(twin, RowsToInsertDeltas(rows));
  }

  // =====================================================================
  // HB2b — aggregates under AggsFaithful (intentional SPI residual)
  // =====================================================================

  ghost function AggsToExpectedStore(
    aggs: seq<Twin.DayGroupAggregate>, days_filter: set<int>
  ): Store
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
  {
    Flatten(Twin.ConsumeAggregates(Twin.EmptyStore(), aggs, days_filter))
  }

  ghost function FilterTableByDays(t: seq<Row>, days_filter: set<int>): seq<Row>
    requires RowsOk(t)
    ensures RowsOk(FilterTableByDays(t, days_filter))
    decreases |t|
  {
    if |t| == 0 then
      []
    else if days_filter != {} && t[0].shipdate !in days_filter then
      FilterTableByDays(t[1..], days_filter)
    else
      [t[0]] + FilterTableByDays(t[1..], days_filter)
  }

  ghost predicate AggsFaithful(
    aggs: seq<Twin.DayGroupAggregate>,
    days_filter: set<int>,
    table: seq<Row>
  )
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    requires RowsOk(table)
  {
    AggsToExpectedStore(aggs, days_filter) == FoldTwin(FilterTableByDays(table, days_filter))
  }

  lemma BuildState_aggs_for_days_RebuildOracleCorrect(
    aggs: seq<Twin.DayGroupAggregate>,
    days_filter: set<int>,
    table: seq<Row>
  )
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    requires RowsOk(table)
    requires days_filter != {}
    requires AggsFaithful(aggs, days_filter, table)
    ensures var rebuilt := Twin.BuildStateForDays(
              days_filter, aggs, true, [], false, []);
            RebuildOracleCorrect(rebuilt, FoldTwin(FilterTableByDays(table, days_filter)))
  {
  }

  lemma BuildState_aggs_all_RebuildOracleCorrect(
    aggs: seq<Twin.DayGroupAggregate>,
    days_filter: set<int>,
    table: seq<Row>
  )
    requires forall i :: 0 <= i < |aggs| ==>
              0 <= aggs[i].returnflag < 256 && 0 <= aggs[i].linestatus < 256
    requires RowsOk(table)
    requires AggsFaithful(aggs, days_filter, table)
    ensures var rebuilt := Twin.BuildStateForDays(
              days_filter, [], false, aggs, true, []);
            RebuildOracleCorrect(rebuilt, FoldTwin(FilterTableByDays(table, days_filter)))
  {
  }

  // =====================================================================
  // HB4 — delete packaging via Fold (not reverse-seq identity)
  // =====================================================================

  ghost function RowsToDeleteDeltas(rows: seq<Row>): seq<Twin.LineitemDelta>
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    decreases |rows|
  {
    if |rows| == 0 then []
    else [RowToDelta(rows[0], -1)] + RowsToDeleteDeltas(rows[1..])
  }

  lemma ApplyTwinDelta_eq_ApplyDelta_delete(store: Store, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures ApplyTwinDelta(store, RowToDelta(r, -1)) == ApplyDelta(store, r, -1)
  {
  }

  lemma ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_delete(store: Store, rows: seq<Row>)
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    ensures ApplyTwinDeltaSeq(store, RowsToDeleteDeltas(rows))
         == ApplyDeltaSeq(store, rows, -1)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      ApplyTwinDelta_eq_ApplyDelta_delete(store, rows[0]);
      ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_delete(ApplyDelta(store, rows[0], -1), rows[1..]);
    }
  }

  lemma RemoveAt_prefix_head(prefix: seq<Row>, rows: seq<Row>)
    requires |rows| > 0
    requires RowsOk(prefix)
    requires RowsOk(rows)
    ensures RemoveAt(prefix + rows, |prefix|) == prefix + rows[1..]
  {
    var t := prefix + rows;
    var i := |prefix|;
    assert t[i] == rows[0];
    assert t[..i] == prefix;
    assert t[i + 1..] == rows[1..];
    assert RemoveAt(t, i) == t[..i] + t[i + 1..];
  }

  lemma ApplyDeltaSeq_delete_suffix_eq_Fold(prefix: seq<Row>, rows: seq<Row>)
    requires RowsOk(prefix)
    requires RowsOk(rows)
    ensures ApplyDeltaSeq(Fold(prefix + rows), rows, -1) == Fold(prefix)
    decreases |rows|
  {
    assert RowsOk(prefix + rows);
    if |rows| == 0 {
      assert prefix + [] == prefix;
    } else {
      var t := prefix + rows;
      RebuildEstablishesInv(t);
      assert Inv(Fold(t), t);
      var i := |prefix|;
      assert 0 <= i < |t|;
      assert t[i] == rows[0];
      InvPreservedByDeleteAt(Fold(t), t, i);
      RemoveAt_prefix_head(prefix, rows);
      assert RemoveAt(t, i) == prefix + rows[1..];
      assert ApplyDelta(Fold(t), rows[0], -1) == Fold(prefix + rows[1..]);
      assert RowsOk(rows[1..]);
      ApplyDeltaSeq_delete_suffix_eq_Fold(prefix, rows[1..]);
    }
  }

  lemma TwinMaintRefinesAux_DeleteSuffix(
    twin: Twin.DayStore,
    s: AuxState,
    rows: seq<Row>
  )
    requires TwinAuxRel(twin, s.store)
    requires StateInv(s)
    requires ActionOk(s, ApplyDeleteSuffix(rows))
    ensures TwinAuxRel(
              Twin.ApplyDeltaSeq(twin, RowsToDeleteDeltas(rows)),
              ApplyMaint(s, ApplyDeleteSuffix(rows)).store)
  {
    assert s.table[|s.table| - |rows|..] == rows;
    var prefix := s.table[..|s.table| - |rows|];
    assert s.table == prefix + rows;
    assert RowsOk(s.table);
    assert RowsOk(prefix);
    assert RowsOk(rows);
    // Aux: ApplyDeltaSeqRev → Fold(prefix)
    ApplyDeltaSeqRevDeleteSuffix(s.store, prefix, rows);
    assert ApplyMaint(s, ApplyDeleteSuffix(rows)).store
        == ApplyDeltaSeqRev(s.store, rows, -1);
    assert Inv(ApplyDeltaSeqRev(s.store, rows, -1), prefix);
    assert ApplyDeltaSeqRev(s.store, rows, -1) == Fold(prefix);
    // Twin: Flatten(ApplyDeltaSeq(...)) == ApplyDeltaSeq(s.store, rows, -1) == Fold(prefix)
    TwinMaintRefinesAux_ApplyDeltaSeq(twin, s.store, RowsToDeleteDeltas(rows));
    ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_delete(s.store, rows);
    assert s.store == Fold(s.table);
    ApplyDeltaSeq_delete_suffix_eq_Fold(prefix, rows);
    assert ApplyDeltaSeq(s.store, rows, -1) == Fold(prefix);
  }

  lemma TwinMaintRefinesAux_InsertSeq(
    twin: Twin.DayStore,
    s: AuxState,
    rows: seq<Row>
  )
    requires TwinAuxRel(twin, s.store)
    requires StateInv(s)
    requires ActionOk(s, ApplyInsertSeq(rows))
    ensures TwinAuxRel(
              Twin.ApplyDeltaSeq(twin, RowsToInsertDeltas(rows)),
              ApplyMaint(s, ApplyInsertSeq(rows)).store)
  {
    TwinMaintRefinesAux_ApplyDeltaSeq(twin, s.store, RowsToInsertDeltas(rows));
    // Insert: ApplyTwinDeltaSeq of +1 deltas == ApplyDeltaSeq(..., 1)
    if |rows| == 0 {
    } else {
      ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_insert_all(s.store, rows);
    }
  }

  lemma ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_insert_all(store: Store, rows: seq<Row>)
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    ensures ApplyTwinDeltaSeq(store, RowsToInsertDeltas(rows))
         == ApplyDeltaSeq(store, rows, 1)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      ApplyTwinDelta_eq_ApplyDelta_insert(store, rows[0]);
      ApplyTwinDeltaSeq_eq_ApplyDeltaSeq_insert_all(ApplyDelta(store, rows[0], 1), rows[1..]);
    }
  }

  // =====================================================================
  // HB1 — requires_rebuild: store refine = Aux NoOp (Qed); action retag
  // =====================================================================

  lemma TwinMaintRefinesAux_RequiresRebuild_store_NoOp(
    buf: Twin.TxnBuffer,
    decision: Twin.MaintenanceAction,
    snapshot: Twin.DayStore,
    rebuilt: Twin.DayStore,
    aux_store: Store
  )
    requires buf.requires_rebuild
    requires TwinAuxRel(snapshot, aux_store)
    ensures Twin.StageMaintenance(buf, decision, snapshot, rebuilt).state == snapshot
    ensures TwinAuxRel(
              Twin.StageMaintenance(buf, decision, snapshot, rebuilt).state,
              aux_store)
    ensures Twin.StageMaintenance(buf, decision, snapshot, rebuilt).action
         == (if decision == Twin.ApplyDelta then Twin.RebuildAll else decision)
  {
    Twin.StageMaintenanceRebuildFlag(buf, decision, snapshot, rebuilt);
  }

  lemma TwinMaintRefinesAux_RequiresRebuild_matches_AuxNoOp(
    buf: Twin.TxnBuffer,
    snapshot: Twin.DayStore,
    rebuilt: Twin.DayStore,
    s: AuxState
  )
    requires buf.requires_rebuild
    requires TwinAuxRel(snapshot, s.store)
    requires StateInv(s)
    ensures TwinAuxRel(
              Twin.StageMaintenance(buf, Twin.ApplyDelta, snapshot, rebuilt).state,
              ApplyMaint(s, NoOp).store)
    ensures ApplyMaint(s, NoOp).store == s.store
    ensures Twin.StageMaintenance(buf, Twin.ApplyDelta, snapshot, rebuilt).action
         == Twin.RebuildAll
  {
    TwinMaintRefinesAux_RequiresRebuild_store_NoOp(
      buf, Twin.ApplyDelta, snapshot, rebuilt, s.store);
  }
}
