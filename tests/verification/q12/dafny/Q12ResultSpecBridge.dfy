// Q12ResultSpecBridge — Inv ⇒ ExecSeq == EvalResultSpec (exact ints).
//
// Reuses GENERIC SortedUniqueMap for ≤2-mode ordering via int ranks.
// Q12-SPECIFIC: join lookup, CASE high/low contrib, shipmode key.

include "generated/Q12ResultSpec.dfy"
include "Q12Aux.dfy"
include "../../patterns/dafny/SortedUniqueMap.dfy"

module Q12ResultSpecBridge {

  import Spec = Q12ResultSpec
  import opened Q12Aux
  import SU = SortedUniqueMap

  ghost function AuxOrderToSpec(o: OrderRow): Spec.OrderRow {
    Spec.OrderRow(o.orderkey, o.priority)
  }

  ghost function AuxLineToSpec(l: LineRow): Spec.LineRow {
    Spec.LineRow(l.orderkey, l.shipmode, l.commitdate, l.receiptdate, l.shipdate)
  }

  ghost function AuxOrdersToSpec(t: seq<OrderRow>): seq<Spec.OrderRow>
    decreases |t|
  {
    if |t| == 0 then []
    else AuxOrdersToSpec(t[..|t| - 1]) + [AuxOrderToSpec(t[|t| - 1])]
  }

  ghost function AuxLinesToSpec(t: seq<LineRow>): seq<Spec.LineRow>
    decreases |t|
  {
    if |t| == 0 then []
    else AuxLinesToSpec(t[..|t| - 1]) + [AuxLineToSpec(t[|t| - 1])]
  }

  ghost function AuxStatsToSpec(s: Stats): Spec.SpecStats {
    Spec.SpecStats(s.high, s.low)
  }

  lemma Classify_eq(p: string)
    ensures ClassifyPriority(p).High? <==> Spec.ClassifyPriority(p).High?
    ensures ClassifyPriority(p).Low? <==> Spec.ClassifyPriority(p).Low?
  {
  }

  lemma LineQualifies_eq(l: LineRow)
    ensures LineQualifies(l) <==> Spec.LineQualifies(AuxLineToSpec(l))
  {
  }

  // ----- Thin Q12 morphisms -----

  lemma KeyMatchesSpec(l: LineRow)
    ensures l.shipmode == AuxLineToSpec(l).shipmode
  {
  }

  lemma FilterMatchesSpec(l: LineRow, start: int, endExclusive: int, m1: string, m2: string)
    ensures (LineQualifies(l) && start <= l.receiptdate < endExclusive &&
             (l.shipmode == m1 || l.shipmode == m2))
         <==>
            (Spec.LineQualifies(AuxLineToSpec(l)) &&
             Spec.InWindow(AuxLineToSpec(l).receiptdate, start, endExclusive) &&
             Spec.ModeAccepted(AuxLineToSpec(l).shipmode, m1, m2))
  {
    LineQualifies_eq(l);
  }

  lemma HighLowContribMatchesSpec(s: Stats, p: Priority, sign: int)
    requires p.High? || p.Low?
    ensures AuxStatsToSpec(ApplyContribution(s, p, sign))
         == Spec.SpecApply(AuxStatsToSpec(s),
              if p.High? then Spec.High else Spec.Low, sign)
  {
  }

  lemma PriorityMap_eq(orders: seq<OrderRow>)
    ensures forall k ::
              (k in FoldPriorities(orders) <==>
               k in Spec.PriorityMap(AuxOrdersToSpec(orders))) &&
              (k in FoldPriorities(orders) ==>
                (FoldPriorities(orders)[k].High? <==>
                 Spec.PriorityMap(AuxOrdersToSpec(orders))[k].High?) &&
                (FoldPriorities(orders)[k].Low? <==>
                 Spec.PriorityMap(AuxOrdersToSpec(orders))[k].Low?))
    decreases |orders|
  {
    if |orders| == 0 {
    } else {
      PriorityMap_eq(orders[..|orders| - 1]);
      Classify_eq(orders[|orders| - 1].priority);
    }
  }

  lemma AuxLinesToSpec_snoc(lines: seq<LineRow>, l: LineRow)
    ensures AuxLinesToSpec(lines + [l]) == AuxLinesToSpec(lines) + [AuxLineToSpec(l)]
  {
  }

  lemma AuxLinesToSpec_unsoc(lines: seq<LineRow>, l: LineRow)
    requires |lines| > 0 && lines[|lines| - 1] == l
    ensures AuxLinesToSpec(lines) == AuxLinesToSpec(lines[..|lines| - 1]) + [AuxLineToSpec(l)]
  {
  }

  ghost function AuxPriToSpec(p: Priority): Spec.Priority
    requires p.High? || p.Low?
  {
    if p.High? then Spec.High else Spec.Low
  }

  lemma ComposeLinesDirect_step(orders: seq<OrderRow>, lines: seq<LineRow>, l: LineRow,
                                start: int, endExclusive: int, m1: string, m2: string)
    ensures ComposeLinesDirect(orders, lines + [l], start, endExclusive, m1, m2)
         == (var pref := ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2);
             var pri := FoldPriorities(orders);
             if !(LineQualifies(l) && start <= l.receiptdate < endExclusive &&
                  (l.shipmode == m1 || l.shipmode == m2) && l.orderkey in pri) then
               pref
             else
               var p := pri[l.orderkey];
               if p.Absent? || p.Neither? then pref
               else
                 var cur := if l.shipmode in pref then pref[l.shipmode] else EmptyStats();
                 pref[l.shipmode := ApplyContribution(cur, p, 1)])
  {
  }

  lemma SpecFoldLines_step(orders: seq<Spec.OrderRow>, lines: seq<Spec.LineRow>, l: Spec.LineRow,
                           start: int, endExclusive: int, m1: string, m2: string)
    ensures Spec.SpecView(orders, lines + [l], start, endExclusive, m1, m2)
         == Spec.ApplyLine(Spec.SpecView(orders, lines, start, endExclusive, m1, m2),
                           Spec.PriorityMap(orders), l, start, endExclusive, m1, m2, 1)
  {
  }

  lemma Compose_eq_SpecView(orders: seq<OrderRow>, lines: seq<LineRow>,
                            start: int, endExclusive: int, m1: string, m2: string)
    ensures forall mode ::
              (mode in ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2)
               <==> mode in Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines),
                                          start, endExclusive, m1, m2)) &&
              (mode in ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2) ==>
                AuxStatsToSpec(ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2)[mode])
                == Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines),
                                 start, endExclusive, m1, m2)[mode])
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      var l := lines[|lines| - 1];
      Compose_eq_SpecView(orders, pref, start, endExclusive, m1, m2);
      PriorityMap_eq(orders);
      LineQualifies_eq(l);
      FilterMatchesSpec(l, start, endExclusive, m1, m2);
      AuxLinesToSpec_unsoc(lines, l);
      var cm0 := ComposeLinesDirect(orders, pref, start, endExclusive, m1, m2);
      var sm0 := Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(pref),
                               start, endExclusive, m1, m2);
      var pri := FoldPriorities(orders);
      var spri := Spec.PriorityMap(AuxOrdersToSpec(orders));
      var cm := ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2);
      var sm := Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines),
                              start, endExclusive, m1, m2);
      // Both sides either skip l or apply the same High/Low +1 to shipmode.
      if LineQualifies(l) && start <= l.receiptdate < endExclusive &&
         (l.shipmode == m1 || l.shipmode == m2) && l.orderkey in pri {
        assert l.orderkey in spri;
        var p := pri[l.orderkey];
        assert p.High? || p.Low?;  // FoldPriorities only stores High/Low
        assert (spri[l.orderkey].High? <==> p.High?) &&
               (spri[l.orderkey].Low? <==> p.Low?);
        HighLowContribMatchesSpec(
          if l.shipmode in cm0 then cm0[l.shipmode] else EmptyStats(), p, 1);
        assert cm == (var cur := if l.shipmode in cm0 then cm0[l.shipmode] else EmptyStats();
                      cm0[l.shipmode := ApplyContribution(cur, p, 1)]);
        assert sm == Spec.ApplyLine(sm0, spri, AuxLineToSpec(l),
                                    start, endExclusive, m1, m2, 1);
      } else {
        assert cm == cm0;
        assert sm == sm0;
      }
    }
  }

  lemma FinalizeMatchesSpec(mode: string, s: Stats)
    ensures |FinalizeOne(mode, s)| == |Spec.SpecFinalizeOne(mode, AuxStatsToSpec(s))|
    ensures |FinalizeOne(mode, s)| > 0 ==>
              FinalizeOne(mode, s)[0].shipmode == Spec.SpecFinalizeOne(mode, AuxStatsToSpec(s))[0].shipmode &&
              FinalizeOne(mode, s)[0].high == Spec.SpecFinalizeOne(mode, AuxStatsToSpec(s))[0].high &&
              FinalizeOne(mode, s)[0].low == Spec.SpecFinalizeOne(mode, AuxStatsToSpec(s))[0].low
  {
  }

  ghost predicate ExactEq(accel: seq<OutRow>, spec: seq<Spec.SpecOutRow>) {
    |accel| == |spec| &&
    forall i :: 0 <= i < |accel| ==>
      accel[i].shipmode == spec[i].shipmode &&
      accel[i].high == spec[i].high &&
      accel[i].low == spec[i].low
  }

  // Rank shipmode for SortedUniqueMap (injective on {m1,m2}).
  ghost function ModeRank(mode: string, m1: string, m2: string): int {
    if mode == m1 && mode == m2 then 0
    else if mode == m1 then (if m1 < m2 then 0 else 1)
    else if mode == m2 then (if m1 < m2 then 1 else 0)
    else 2
  }

  ghost function ExactEntries(rows: seq<OutRow>, m1: string, m2: string): seq<SU.Entry<(int, int)>>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SU.Entry(ModeRank(rows[0].shipmode, m1, m2), (rows[0].high, rows[0].low))]
         + ExactEntries(rows[1..], m1, m2)
  }

  ghost function SpecExactEntries(rows: seq<Spec.SpecOutRow>, m1: string, m2: string): seq<SU.Entry<(int, int)>>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SU.Entry(ModeRank(rows[0].shipmode, m1, m2), (rows[0].high, rows[0].low))]
         + SpecExactEntries(rows[1..], m1, m2)
  }

  lemma Collect_eq(orders: seq<OrderRow>, lines: seq<LineRow>,
                   start: int, endExclusive: int, m1: string, m2: string)
    ensures ExactEq(
      CollectModes(ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2), m1, m2),
      Spec.CollectModes(m1, m2,
        Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines), start, endExclusive, m1, m2)))
  {
    Compose_eq_SpecView(orders, lines, start, endExclusive, m1, m2);
    var cm := ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2);
    var sm := Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines), start, endExclusive, m1, m2);
    // GetKey alignment from Compose_eq_SpecView
    assert AuxStatsToSpec(if m1 in cm then cm[m1] else EmptyStats())
        == Spec.GetKey(sm, m1) by {
      if m1 in cm {
        assert m1 in sm;
        assert AuxStatsToSpec(cm[m1]) == sm[m1];
      } else {
        assert !(m1 in sm);
      }
    }
    FinalizeMatchesSpec(m1, if m1 in cm then cm[m1] else EmptyStats());
    if m1 != m2 {
      assert AuxStatsToSpec(if m2 in cm then cm[m2] else EmptyStats())
          == Spec.GetKey(sm, m2) by {
        if m2 in cm {
          assert m2 in sm;
          assert AuxStatsToSpec(cm[m2]) == sm[m2];
        } else {
          assert !(m2 in sm);
        }
      }
      FinalizeMatchesSpec(m2, if m2 in cm then cm[m2] else EmptyStats());
    }
  }

  lemma Sort_preserves_ExactEq(a: seq<OutRow>, b: seq<Spec.SpecOutRow>)
    requires ExactEq(a, b)
    requires |a| <= 2  // Q12 CollectModes emits ≤2 rows
    ensures ExactEq(SortByMode(a), Spec.SortByMode(b))
  {
    if |a| == 0 {
    } else if |a| == 1 {
    } else {
      // |a| == 2
      assert |b| == 2;
      assert a[0].shipmode == b[0].shipmode && a[1].shipmode == b[1].shipmode;
      assert a[0].high == b[0].high && a[0].low == b[0].low;
      assert a[1].high == b[1].high && a[1].low == b[1].low;
    }
  }

  // ===== Main execution theorem (exact) =====
  lemma Q12ExecCorrect(s: State, orders: seq<OrderRow>, lines: seq<LineRow>,
                       start: int, endExclusive: int, m1: string, m2: string)
    requires Inv(s, orders, lines)
    ensures ExactEq(
              ExecSeq(orders, lines, start, endExclusive, m1, m2),
              Spec.EvalResultSpec(AuxOrdersToSpec(orders), AuxLinesToSpec(lines),
                                  start, endExclusive, m1, m2))
  {
    Collect_eq(orders, lines, start, endExclusive, m1, m2);
    var ca := CollectModes(ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2), m1, m2);
    var cs := Spec.CollectModes(m1, m2,
      Spec.SpecView(AuxOrdersToSpec(orders), AuxLinesToSpec(lines), start, endExclusive, m1, m2));
    assert |ca| <= 2;
    Sort_preserves_ExactEq(ca, cs);
  }
}
