// Q12 accelerator algebra — mirrors postgres aux BuildState / FinalizeAndEmit.
//
// Inv(A, Orders, Lines) ⇔ A = BuildFold(Orders, Lines)
// Order ApplyDelta: ReclassifyOrder (semantic model of C++ reverse-index path).

module Q12Aux {

  datatype Priority = Absent | High | Low | Neither

  datatype Stats = Stats(high: int, low: int)
  datatype OrderRow = OrderRow(orderkey: int, priority: string)
  datatype LineRow = LineRow(
    orderkey: int,
    shipmode: string,
    commitdate: int,
    receiptdate: int,
    shipdate: int
  )

  datatype OutRow = OutRow(shipmode: string, high: int, low: int)

  datatype CellKey = CellKey(day: int, mode: string)
  type DayStore = map<CellKey, Stats>
  type PriorityStore = map<int, Priority>

  datatype State = State(priorities: PriorityStore, days: DayStore)

  ghost function EmptyStats(): Stats { Stats(0, 0) }

  ghost function ClassifyPriority(p: string): Priority {
    if p == "1-URGENT" || p == "2-HIGH" then High else Low
  }

  ghost predicate LineQualifies(l: LineRow) {
    l.commitdate < l.receiptdate && l.shipdate < l.commitdate
  }

  ghost function GetDay(store: DayStore, k: CellKey): Stats {
    if k in store then store[k] else EmptyStats()
  }

  ghost function ApplyContribution(s: Stats, p: Priority, sign: int): Stats {
    if p.High? then Stats(s.high + sign, s.low)
    else if p.Low? then Stats(s.high, s.low + sign)
    else s
  }

  ghost function ApplyLineDelta(store: DayStore, pri: PriorityStore, l: LineRow, sign: int): DayStore {
    if !LineQualifies(l) then store
    else if !(l.orderkey in pri) then store
    else
      var p := pri[l.orderkey];
      if p.Absent? || p.Neither? then store
      else
        var k := CellKey(l.receiptdate, l.shipmode);
        var s2 := ApplyContribution(GetDay(store, k), p, sign);
        if s2.high == 0 && s2.low == 0 then store - {k}
        else store[k := s2]
  }

  ghost function FoldPriorities(orders: seq<OrderRow>): PriorityStore
    decreases |orders|
  {
    if |orders| == 0 then map[]
    else
      var pref := FoldPriorities(orders[..|orders| - 1]);
      var o := orders[|orders| - 1];
      pref[o.orderkey := ClassifyPriority(o.priority)]
  }

  ghost function FoldLines(store: DayStore, pri: PriorityStore, lines: seq<LineRow>): DayStore
    decreases |lines|
  {
    if |lines| == 0 then store
    else
      ApplyLineDelta(FoldLines(store, pri, lines[..|lines| - 1]), pri, lines[|lines| - 1], 1)
  }

  ghost function BuildFold(orders: seq<OrderRow>, lines: seq<LineRow>): State {
    var pri := FoldPriorities(orders);
    State(pri, FoldLines(map[], pri, lines))
  }

  ghost predicate Inv(s: State, orders: seq<OrderRow>, lines: seq<LineRow>) {
    s == BuildFold(orders, lines)
  }

  ghost function AccMode(acc: map<string, Stats>, mode: string, s: Stats): map<string, Stats> {
    var cur := if mode in acc then acc[mode] else EmptyStats();
    acc[mode := Stats(cur.high + s.high, cur.low + s.low)]
  }

  ghost function FoldDayKeys(keys: seq<CellKey>, days: DayStore, start: int, endExclusive: int,
                             m1: string, m2: string, acc: map<string, Stats>): map<string, Stats>
    decreases |keys|
  {
    if |keys| == 0 then acc
    else
      var k := keys[0];
      var acc2 :=
        if start <= k.day < endExclusive && (k.mode == m1 || k.mode == m2) && k in days then
          AccMode(acc, k.mode, days[k])
        else acc;
      FoldDayKeys(keys[1..], days, start, endExclusive, m1, m2, acc2)
  }

  ghost function ComposeFrom(s: State, start: int, endExclusive: int, m1: string, m2: string): map<string, Stats> {
    FoldDayKeys(SetToSeq(s.days.Keys), s.days, start, endExclusive, m1, m2, map[])
  }

  ghost function Compose(orders: seq<OrderRow>, lines: seq<LineRow>,
                         start: int, endExclusive: int, m1: string, m2: string): map<string, Stats>
  {
    ComposeFrom(BuildFold(orders, lines), start, endExclusive, m1, m2)
  }

  ghost function ComposeLinesDirect(orders: seq<OrderRow>, lines: seq<LineRow>,
                                    start: int, endExclusive: int, m1: string, m2: string): map<string, Stats>
    decreases |lines|
  {
    if |lines| == 0 then map[]
    else
      var pref := ComposeLinesDirect(orders, lines[..|lines| - 1], start, endExclusive, m1, m2);
      var l := lines[|lines| - 1];
      var pri := FoldPriorities(orders);
      if !(LineQualifies(l) && start <= l.receiptdate < endExclusive &&
           (l.shipmode == m1 || l.shipmode == m2) && l.orderkey in pri) then
        pref
      else
        var p := pri[l.orderkey];
        if p.Absent? || p.Neither? then pref
        else
          var cur := if l.shipmode in pref then pref[l.shipmode] else EmptyStats();
          pref[l.shipmode := ApplyContribution(cur, p, 1)]
  }

  ghost function FinalizeOne(mode: string, s: Stats): seq<OutRow> {
    if s.high == 0 && s.low == 0 then []
    else [OutRow(mode, s.high, s.low)]
  }

  ghost function InsertSorted(rows: seq<OutRow>, row: OutRow): seq<OutRow>
    decreases |rows|
  {
    if |rows| == 0 then [row]
    else if row.shipmode < rows[0].shipmode then [row] + rows
    else [rows[0]] + InsertSorted(rows[1..], row)
  }

  ghost function SortByMode(rows: seq<OutRow>): seq<OutRow>
    decreases |rows|
  {
    if |rows| == 0 then []
    else InsertSorted(SortByMode(rows[1..]), rows[0])
  }

  ghost function CollectModes(m: map<string, Stats>, m1: string, m2: string): seq<OutRow> {
    var r1 := FinalizeOne(m1, if m1 in m then m[m1] else EmptyStats());
    if m1 == m2 then r1
    else r1 + FinalizeOne(m2, if m2 in m then m[m2] else EmptyStats())
  }

  ghost function ExecSeq(orders: seq<OrderRow>, lines: seq<LineRow>,
                         start: int, endExclusive: int, m1: string, m2: string): seq<OutRow> {
    SortByMode(CollectModes(ComposeLinesDirect(orders, lines, start, endExclusive, m1, m2), m1, m2))
  }

  ghost function SetToSeq<T>(s: set<T>): seq<T>
    decreases s
  {
    if s == {} then []
    else
      var x :| x in s;
      [x] + SetToSeq(s - {x})
  }

  lemma RebuildEstablishesInv(orders: seq<OrderRow>, lines: seq<LineRow>)
    ensures Inv(BuildFold(orders, lines), orders, lines)
  {
  }

  lemma FoldLines_snoc(store: DayStore, pri: PriorityStore, lines: seq<LineRow>, l: LineRow)
    ensures FoldLines(store, pri, lines + [l])
         == ApplyLineDelta(FoldLines(store, pri, lines), pri, l, 1)
  {
  }

  lemma InvPreservedByLineInsert(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, l: LineRow)
    requires Inv(s, orders, lines)
    ensures Inv(State(s.priorities, ApplyLineDelta(s.days, s.priorities, l, 1)),
                orders, lines + [l])
  {
    assert FoldPriorities(orders) == s.priorities;
    assert s.days == FoldLines(map[], s.priorities, lines);
    FoldLines_snoc(map[], s.priorities, lines, l);
  }

  lemma FoldLines_unsoc(store: DayStore, pri: PriorityStore, lines: seq<LineRow>, l: LineRow)
    requires |lines| > 0 && lines[|lines| - 1] == l
    ensures FoldLines(store, pri, lines)
         == ApplyLineDelta(FoldLines(store, pri, lines[..|lines| - 1]), pri, l, 1)
  {
  }

  ghost predicate CellNonEmpty(s: Stats) {
    !(s.high == 0 && s.low == 0)
  }

  ghost predicate StatsNonNeg(s: Stats) {
    s.high >= 0 && s.low >= 0
  }

  lemma ApplyContribution_plus1_nonempty(s: Stats, p: Priority)
    requires p.High? || p.Low?
    requires StatsNonNeg(s)
    ensures CellNonEmpty(ApplyContribution(s, p, 1))
    ensures StatsNonNeg(ApplyContribution(s, p, 1))
  {
    if p.High? {
      assert ApplyContribution(s, p, 1) == Stats(s.high + 1, s.low);
    } else {
      assert ApplyContribution(s, p, 1) == Stats(s.high, s.low + 1);
    }
  }

  lemma ApplyContributionCancel(s: Stats, p: Priority)
    requires p.High? || p.Low?
    ensures ApplyContribution(ApplyContribution(s, p, 1), p, -1) == s
  {
  }

  lemma FoldLinesHasNoEmptyCell(store: DayStore, pri: PriorityStore, lines: seq<LineRow>, k: CellKey)
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures var f := FoldLines(store, pri, lines);
            k !in f || (CellNonEmpty(f[k]) && StatsNonNeg(f[k]))
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      var l := lines[|lines| - 1];
      var mid := FoldLines(store, pri, pref);
      FoldLinesHasNoEmptyCell(store, pri, pref, k);
      FoldLinesHasNoEmptyCell(store, pri, pref, CellKey(l.receiptdate, l.shipmode));
      if LineQualifies(l) && l.orderkey in pri {
        var p := pri[l.orderkey];
        if p.High? || p.Low? {
          var ck := CellKey(l.receiptdate, l.shipmode);
          var s0 := GetDay(mid, ck);
          assert StatsNonNeg(s0) by {
            if ck in mid {
              FoldLinesHasNoEmptyCell(store, pri, pref, ck);
            } else {
              assert s0 == EmptyStats();
            }
          }
          ApplyContribution_plus1_nonempty(s0, p);
          var s1 := ApplyContribution(s0, p, 1);
          assert FoldLines(store, pri, lines) == mid[ck := s1];
        }
      }
    }
  }

  lemma FoldLines_all_nonempty(store: DayStore, pri: PriorityStore, lines: seq<LineRow>)
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures forall c :: c in FoldLines(store, pri, lines) ==>
              CellNonEmpty(FoldLines(store, pri, lines)[c]) &&
              StatsNonNeg(FoldLines(store, pri, lines)[c])
  {
    forall c | c in FoldLines(store, pri, lines)
      ensures CellNonEmpty(FoldLines(store, pri, lines)[c]) &&
              StatsNonNeg(FoldLines(store, pri, lines)[c])
    {
      FoldLinesHasNoEmptyCell(store, pri, lines, c);
    }
  }

  lemma ApplyLineDeltaCancel(store: DayStore, pri: PriorityStore, l: LineRow)
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures ApplyLineDelta(ApplyLineDelta(store, pri, l, 1), pri, l, -1) == store
  {
    if LineQualifies(l) && l.orderkey in pri {
      var p := pri[l.orderkey];
      if p.High? || p.Low? {
        var k := CellKey(l.receiptdate, l.shipmode);
        var s0 := GetDay(store, k);
        assert StatsNonNeg(s0) by {
          if k in store {
          } else {
            assert s0 == EmptyStats();
          }
        }
        ApplyContributionCancel(s0, p);
        ApplyContribution_plus1_nonempty(s0, p);
        var s1 := ApplyContribution(s0, p, 1);
        var mid := store[k := s1];
        if !CellNonEmpty(s0) {
          // s0 empty ⇒ k ∉ store; +1 writes k; −1 clears k back to store
          assert k !in store && s0 == EmptyStats();
          assert ApplyLineDelta(store, pri, l, 1) == mid;
          assert ApplyContribution(s1, p, -1) == EmptyStats();
          assert ApplyLineDelta(mid, pri, l, -1) == mid - {k};
          assert mid - {k} == store;
        } else {
          assert k in store && store[k] == s0;
          assert ApplyLineDelta(store, pri, l, 1) == mid;
          assert ApplyLineDelta(mid, pri, l, -1) == mid[k := s0];
          assert mid[k := s0] == store;
        }
      }
    }
  }

  lemma InvPreservedByLineDeleteLast(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, l: LineRow)
    requires Inv(s, orders, lines)
    requires |lines| > 0 && lines[|lines| - 1] == l
    ensures Inv(State(s.priorities, ApplyLineDelta(s.days, s.priorities, l, -1)),
                orders, lines[..|lines| - 1])
  {
    var pri := s.priorities;
    var pref := lines[..|lines| - 1];
    FoldLines_unsoc(map[], pri, lines, l);
    assert s.days == ApplyLineDelta(FoldLines(map[], pri, pref), pri, l, 1);
    FoldLines_all_nonempty(map[], pri, pref);
    ApplyLineDeltaCancel(FoldLines(map[], pri, pref), pri, l);
  }


  // ----- Order reclassify (C++ reverse-index ApplyDelta model) -----
  // Line scan ≡ reverse-index contents.

  ghost function GetPriority(pri: PriorityStore, orderkey: int): Priority {
    if orderkey in pri then pri[orderkey] else Absent
  }

  ghost function SetPriority(pri: PriorityStore, orderkey: int, p: Priority): PriorityStore {
    if p.Absent? then pri - {orderkey} else pri[orderkey := p]
  }

  ghost function WriteDay(store: DayStore, k: CellKey, s: Stats): DayStore {
    if s.high == 0 && s.low == 0 then store - {k} else store[k := s]
  }

  ghost function AdjustDaysForOrder(store: DayStore, lines: seq<LineRow>, orderkey: int,
                                    p: Priority, sign: int): DayStore
    decreases |lines|
  {
    if |lines| == 0 then store
    else
      var pref := AdjustDaysForOrder(store, lines[..|lines| - 1], orderkey, p, sign);
      var l := lines[|lines| - 1];
      if !(LineQualifies(l) && l.orderkey == orderkey) then pref
      else if p.Absent? || p.Neither? then pref
      else
        var k := CellKey(l.receiptdate, l.shipmode);
        WriteDay(pref, k, ApplyContribution(GetDay(pref, k), p, sign))
  }

  ghost function ReclassifyOrder(s: State, lines: seq<LineRow>, orderkey: int,
                                 newP: Priority): State {
    var oldP := GetPriority(s.priorities, orderkey);
    if oldP == newP then
      State(SetPriority(s.priorities, orderkey, newP), s.days)
    else
      var d1 := AdjustDaysForOrder(s.days, lines, orderkey, oldP, -1);
      var d2 := AdjustDaysForOrder(d1, lines, orderkey, newP, 1);
      State(SetPriority(s.priorities, orderkey, newP), d2)
  }

  lemma FoldPriorities_snoc(orders: seq<OrderRow>, o: OrderRow)
    ensures FoldPriorities(orders + [o])
         == FoldPriorities(orders)[o.orderkey := ClassifyPriority(o.priority)]
  {
  }

  lemma FoldPriorities_unsoc(orders: seq<OrderRow>, o: OrderRow)
    requires |orders| > 0 && orders[|orders| - 1] == o
    ensures FoldPriorities(orders)
         == FoldPriorities(orders[..|orders| - 1])[o.orderkey := ClassifyPriority(o.priority)]
  {
  }

  lemma FoldPriorities_key_exists(orders: seq<OrderRow>, k: int)
    ensures (k in FoldPriorities(orders)) <==>
            (exists i :: 0 <= i < |orders| && orders[i].orderkey == k)
    decreases |orders|
  {
    if |orders| == 0 {
    } else {
      FoldPriorities_key_exists(orders[..|orders| - 1], k);
    }
  }

  lemma FoldPriorities_fresh(orders: seq<OrderRow>, o: OrderRow)
    requires forall i :: 0 <= i < |orders| ==> orders[i].orderkey != o.orderkey
    ensures o.orderkey !in FoldPriorities(orders)
  {
    FoldPriorities_key_exists(orders, o.orderkey);
  }

  // ----- AdjustDays ≡ FoldLines (ORDER_MAINT_PROVED) -----

  ghost function AddStats(a: Stats, b: Stats): Stats {
    Stats(a.high + b.high, a.low + b.low)
  }

  ghost function UnitContrib(p: Priority): Stats {
    if p.High? then Stats(1, 0) else if p.Low? then Stats(0, 1) else EmptyStats()
  }

  ghost function ScaleStats(s: Stats, n: int): Stats {
    Stats(s.high * n, s.low * n)
  }

  lemma ScaleStats_Add(a: Stats, b: Stats, n: int)
    ensures ScaleStats(AddStats(a, b), n) == AddStats(ScaleStats(a, n), ScaleStats(b, n))
  {
  }

  lemma AddStats_assoc(a: Stats, b: Stats, c: Stats)
    ensures AddStats(AddStats(a, b), c) == AddStats(a, AddStats(b, c))
  {
  }

  lemma AddStats_comm(a: Stats, b: Stats)
    ensures AddStats(a, b) == AddStats(b, a)
  {
  }

  lemma AddStats_zero_r(a: Stats)
    ensures AddStats(a, EmptyStats()) == a
  {
  }

  lemma ApplyContribution_AddUnit(s: Stats, p: Priority, sign: int)
    requires p.High? || p.Low?
    ensures ApplyContribution(s, p, sign) == AddStats(s, ScaleStats(UnitContrib(p), sign))
  {
  }

  lemma WriteDay_get(store: DayStore, k: CellKey, s: Stats, k2: CellKey)
    ensures GetDay(WriteDay(store, k, s), k2) == (if k2 == k then s else GetDay(store, k2))
  {
  }

  ghost function OrderCellContrib(lines: seq<LineRow>, orderkey: int, p: Priority, k: CellKey): Stats
    decreases |lines|
  {
    if |lines| == 0 then EmptyStats()
    else
      var pref := OrderCellContrib(lines[..|lines| - 1], orderkey, p, k);
      var l := lines[|lines| - 1];
      if LineQualifies(l) && l.orderkey == orderkey &&
         CellKey(l.receiptdate, l.shipmode) == k && (p.High? || p.Low?) then
        AddStats(pref, UnitContrib(p))
      else pref
  }

  ghost function LineCellContrib(pri: PriorityStore, l: LineRow, k: CellKey): Stats {
    if LineQualifies(l) && CellKey(l.receiptdate, l.shipmode) == k &&
       l.orderkey in pri && (pri[l.orderkey].High? || pri[l.orderkey].Low?) then
      UnitContrib(pri[l.orderkey])
    else EmptyStats()
  }

  ghost function LinesCellContrib(pri: PriorityStore, lines: seq<LineRow>, k: CellKey): Stats
    decreases |lines|
  {
    if |lines| == 0 then EmptyStats()
    else AddStats(LinesCellContrib(pri, lines[..|lines| - 1], k),
                  LineCellContrib(pri, lines[|lines| - 1], k))
  }

  lemma {:isolate_assertions} AdjustDays_get_step(store: DayStore, lines: seq<LineRow>, orderkey: int,
                                                  p: Priority, sign: int, k: CellKey)
    requires |lines| > 0
    requires p.High? || p.Low?
    requires GetDay(AdjustDaysForOrder(store, lines[..|lines| - 1], orderkey, p, sign), k)
          == AddStats(GetDay(store, k),
                      ScaleStats(OrderCellContrib(lines[..|lines| - 1], orderkey, p, k), sign))
    ensures GetDay(AdjustDaysForOrder(store, lines, orderkey, p, sign), k)
         == AddStats(GetDay(store, k), ScaleStats(OrderCellContrib(lines, orderkey, p, k), sign))
  {
    var pref := lines[..|lines| - 1];
    var l := lines[|lines| - 1];
    var mid := AdjustDaysForOrder(store, pref, orderkey, p, sign);
    if LineQualifies(l) && l.orderkey == orderkey {
      var ck := CellKey(l.receiptdate, l.shipmode);
      var s2 := ApplyContribution(GetDay(mid, ck), p, sign);
      ApplyContribution_AddUnit(GetDay(mid, ck), p, sign);
      assert AdjustDaysForOrder(store, lines, orderkey, p, sign) == WriteDay(mid, ck, s2);
      WriteDay_get(mid, ck, s2, k);
      if k == ck {
        assert OrderCellContrib(lines, orderkey, p, k)
            == AddStats(OrderCellContrib(pref, orderkey, p, k), UnitContrib(p));
        ScaleStats_Add(OrderCellContrib(pref, orderkey, p, k), UnitContrib(p), sign);
        AddStats_assoc(GetDay(store, k),
                       ScaleStats(OrderCellContrib(pref, orderkey, p, k), sign),
                       ScaleStats(UnitContrib(p), sign));
      } else {
        assert OrderCellContrib(lines, orderkey, p, k)
            == OrderCellContrib(pref, orderkey, p, k);
      }
    } else {
      assert AdjustDaysForOrder(store, lines, orderkey, p, sign) == mid;
      assert OrderCellContrib(lines, orderkey, p, k)
          == OrderCellContrib(pref, orderkey, p, k);
    }
  }

  lemma {:isolate_assertions} {:induction false} {:timeLimit 300} AdjustDays_get(store: DayStore, lines: seq<LineRow>, orderkey: int,
                       p: Priority, sign: int, k: CellKey)
    requires p.High? || p.Low?
    ensures GetDay(AdjustDaysForOrder(store, lines, orderkey, p, sign), k)
         == AddStats(GetDay(store, k), ScaleStats(OrderCellContrib(lines, orderkey, p, k), sign))
    decreases |lines|
  {
    if |lines| == 0 {
      assert OrderCellContrib(lines, orderkey, p, k) == EmptyStats();
      assert ScaleStats(EmptyStats(), sign) == EmptyStats();
      assert AdjustDaysForOrder(store, lines, orderkey, p, sign) == store;
      AddStats_zero_r(GetDay(store, k));
    } else {
      var pref := lines[..|lines| - 1];
      AdjustDays_get(store, pref, orderkey, p, sign, k);
      AdjustDays_get_step(store, lines, orderkey, p, sign, k);
      assert GetDay(AdjustDaysForOrder(store, lines, orderkey, p, sign), k)
          == AddStats(GetDay(store, k), ScaleStats(OrderCellContrib(lines, orderkey, p, k), sign));
    }
  }

  // Local (non-inductive) characterization of a single +1 line delta on GetDay.
  // Isolating this keeps the map-update / non-empty reasoning out of the
  // FoldLines_get induction, which otherwise produces an assertion batch too
  // large for the SMT solver (timeout / z3 crash).
  lemma {:isolate_assertions} ApplyLineDelta_get(store: DayStore, pri: PriorityStore, l: LineRow, k: CellKey)
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures GetDay(ApplyLineDelta(store, pri, l, 1), k)
         == AddStats(GetDay(store, k), LineCellContrib(pri, l, k))
  {
    if !LineQualifies(l) || !(l.orderkey in pri) {
      assert LineCellContrib(pri, l, k) == EmptyStats();
      assert ApplyLineDelta(store, pri, l, 1) == store;
      AddStats_zero_r(GetDay(store, k));
    } else {
      var p := pri[l.orderkey];
      if p.Absent? || p.Neither? {
        assert LineCellContrib(pri, l, k) == EmptyStats();
        assert ApplyLineDelta(store, pri, l, 1) == store;
        AddStats_zero_r(GetDay(store, k));
      } else {
        var ck := CellKey(l.receiptdate, l.shipmode);
        var s0 := GetDay(store, ck);
        assert StatsNonNeg(s0) by {
          if ck in store {
          } else {
            assert s0 == EmptyStats();
          }
        }
        ApplyContribution_plus1_nonempty(s0, p);
        var s2 := ApplyContribution(s0, p, 1);
        ApplyContribution_AddUnit(s0, p, 1);
        assert s2 == AddStats(s0, UnitContrib(p));
        assert ApplyLineDelta(store, pri, l, 1) == store[ck := s2];
        assert store[ck := s2] == WriteDay(store, ck, s2);
        WriteDay_get(store, ck, s2, k);
        if k == ck {
          assert LineCellContrib(pri, l, k) == UnitContrib(p);
        } else {
          assert LineCellContrib(pri, l, k) == EmptyStats();
          AddStats_zero_r(GetDay(store, k));
        }
      }
    }
  }

  // One FoldLines step on GetDay — keeps the inductive FoldLines_get thin.
  lemma {:isolate_assertions} FoldLines_get_step(store: DayStore, pri: PriorityStore,
                                                 lines: seq<LineRow>, k: CellKey)
    requires |lines| > 0
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    requires GetDay(FoldLines(store, pri, lines[..|lines| - 1]), k)
          == AddStats(GetDay(store, k), LinesCellContrib(pri, lines[..|lines| - 1], k))
    ensures GetDay(FoldLines(store, pri, lines), k)
         == AddStats(GetDay(store, k), LinesCellContrib(pri, lines, k))
  {
    var pref := lines[..|lines| - 1];
    var l := lines[|lines| - 1];
    var mid := FoldLines(store, pri, pref);
    FoldLines_all_nonempty(store, pri, pref);
    ApplyLineDelta_get(mid, pri, l, k);
    assert FoldLines(store, pri, lines) == ApplyLineDelta(mid, pri, l, 1);
    assert LinesCellContrib(pri, lines, k)
        == AddStats(LinesCellContrib(pri, pref, k), LineCellContrib(pri, l, k));
    AddStats_assoc(GetDay(store, k), LinesCellContrib(pri, pref, k), LineCellContrib(pri, l, k));
  }

  lemma {:isolate_assertions} {:induction false} FoldLines_get(store: DayStore, pri: PriorityStore, lines: seq<LineRow>, k: CellKey)
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures GetDay(FoldLines(store, pri, lines), k)
         == AddStats(GetDay(store, k), LinesCellContrib(pri, lines, k))
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      FoldLines_get(store, pri, pref, k);
      FoldLines_get_step(store, pri, lines, k);
    }
  }

  // Local (non-inductive) step relating a single line's contribution under a
  // fresh-key priority insert.  Extracting the map-update reasoning here keeps
  // the LinesCellContrib_insert induction as pure integer arithmetic.
  lemma LineCellContrib_insertStep(pri: PriorityStore, l: LineRow, orderkey: int, p: Priority, k: CellKey)
    requires orderkey !in pri
    requires p.High? || p.Low?
    ensures LineCellContrib(pri[orderkey := p], l, k)
         == AddStats(LineCellContrib(pri, l, k),
                     if LineQualifies(l) && l.orderkey == orderkey &&
                        CellKey(l.receiptdate, l.shipmode) == k
                     then UnitContrib(p) else EmptyStats())
  {
    var pri2 := pri[orderkey := p];
    if LineQualifies(l) && CellKey(l.receiptdate, l.shipmode) == k {
      if l.orderkey == orderkey {
        assert LineCellContrib(pri, l, k) == EmptyStats();
        assert pri2[l.orderkey] == p;
        assert LineCellContrib(pri2, l, k) == UnitContrib(p);
      } else {
        if l.orderkey in pri {
          assert pri2[l.orderkey] == pri[l.orderkey];
        } else {
          assert l.orderkey !in pri2;
        }
      }
    }
  }

  lemma {:isolate_assertions} {:induction false} LinesCellContrib_insert(pri: PriorityStore, lines: seq<LineRow>,
                                orderkey: int, p: Priority, k: CellKey)
    requires orderkey !in pri
    requires p.High? || p.Low?
    ensures LinesCellContrib(pri[orderkey := p], lines, k)
         == AddStats(LinesCellContrib(pri, lines, k), OrderCellContrib(lines, orderkey, p, k))
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      var l := lines[|lines| - 1];
      var pri2 := pri[orderkey := p];
      var delta := if LineQualifies(l) && l.orderkey == orderkey &&
                      CellKey(l.receiptdate, l.shipmode) == k
                   then UnitContrib(p) else EmptyStats();
      LinesCellContrib_insert(pri, pref, orderkey, p, k);
      LineCellContrib_insertStep(pri, l, orderkey, p, k);
      // LHS = AddStats(Lines(pri2,pref), Line(pri2,l))
      //     = AddStats(AddStats(Lines(pri,pref), Order(pref)), AddStats(Line(pri,l), delta))
      AddStats_assoc(LinesCellContrib(pri, pref, k), OrderCellContrib(pref, orderkey, p, k),
                     AddStats(LineCellContrib(pri, l, k), delta));
      AddStats_assoc(OrderCellContrib(pref, orderkey, p, k), LineCellContrib(pri, l, k), delta);
      AddStats_comm(OrderCellContrib(pref, orderkey, p, k), LineCellContrib(pri, l, k));
      AddStats_assoc(LineCellContrib(pri, l, k), OrderCellContrib(pref, orderkey, p, k), delta);
      AddStats_assoc(LinesCellContrib(pri, pref, k), LineCellContrib(pri, l, k),
                     AddStats(OrderCellContrib(pref, orderkey, p, k), delta));
      assert OrderCellContrib(lines, orderkey, p, k)
          == AddStats(OrderCellContrib(pref, orderkey, p, k), delta);
    }
  }

  // Local (non-inductive) step relating a single line's contribution under a
  // priority remove.
  lemma LineCellContrib_removeStep(pri: PriorityStore, l: LineRow, orderkey: int, p: Priority, k: CellKey)
    requires orderkey in pri && pri[orderkey] == p
    requires p.High? || p.Low?
    ensures LineCellContrib(pri - {orderkey}, l, k)
         == AddStats(LineCellContrib(pri, l, k),
                     ScaleStats(if LineQualifies(l) && l.orderkey == orderkey &&
                                   CellKey(l.receiptdate, l.shipmode) == k
                                then UnitContrib(p) else EmptyStats(), -1))
  {
    var pri2 := pri - {orderkey};
    if LineQualifies(l) && CellKey(l.receiptdate, l.shipmode) == k {
      if l.orderkey == orderkey {
        assert LineCellContrib(pri, l, k) == UnitContrib(p);
        assert LineCellContrib(pri2, l, k) == EmptyStats();
      } else {
        if l.orderkey in pri {
          assert pri2[l.orderkey] == pri[l.orderkey];
        } else {
          assert l.orderkey !in pri2;
        }
      }
    }
  }

  lemma {:isolate_assertions} {:induction false} LinesCellContrib_remove(pri: PriorityStore, lines: seq<LineRow>,
                                orderkey: int, p: Priority, k: CellKey)
    requires orderkey in pri && pri[orderkey] == p
    requires p.High? || p.Low?
    ensures LinesCellContrib(pri - {orderkey}, lines, k)
         == AddStats(LinesCellContrib(pri, lines, k),
                     ScaleStats(OrderCellContrib(lines, orderkey, p, k), -1))
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      var l := lines[|lines| - 1];
      var delta := if LineQualifies(l) && l.orderkey == orderkey &&
                      CellKey(l.receiptdate, l.shipmode) == k
                   then UnitContrib(p) else EmptyStats();
      LinesCellContrib_remove(pri, pref, orderkey, p, k);
      LineCellContrib_removeStep(pri, l, orderkey, p, k);
      ScaleStats_Add(OrderCellContrib(pref, orderkey, p, k), delta, -1);
      assert OrderCellContrib(lines, orderkey, p, k)
          == AddStats(OrderCellContrib(pref, orderkey, p, k), delta);
      var negPref := ScaleStats(OrderCellContrib(pref, orderkey, p, k), -1);
      var negDelta := ScaleStats(delta, -1);
      // LHS = AddStats(Lines(pri2,pref), Line(pri2,l))
      //     = AddStats(AddStats(Lines(pri,pref), negPref), AddStats(Line(pri,l), negDelta))
      AddStats_assoc(LinesCellContrib(pri, pref, k), negPref,
                     AddStats(LineCellContrib(pri, l, k), negDelta));
      AddStats_assoc(negPref, LineCellContrib(pri, l, k), negDelta);
      AddStats_comm(negPref, LineCellContrib(pri, l, k));
      AddStats_assoc(LineCellContrib(pri, l, k), negPref, negDelta);
      AddStats_assoc(LinesCellContrib(pri, pref, k), LineCellContrib(pri, l, k),
                     AddStats(negPref, negDelta));
    }
  }

  lemma DayStore_domain_nonempty(a: DayStore)
    requires forall c :: c in a ==> CellNonEmpty(a[c])
    ensures forall k :: (k in a) <==> CellNonEmpty(GetDay(a, k))
  {
    forall k
      ensures (k in a) <==> CellNonEmpty(GetDay(a, k))
    {
      if k in a {
        assert GetDay(a, k) == a[k];
      } else {
        assert GetDay(a, k) == EmptyStats();
      }
    }
  }

  lemma DayStore_eq_from_get(a: DayStore, b: DayStore)
    requires forall c :: c in a ==> CellNonEmpty(a[c])
    requires forall c :: c in b ==> CellNonEmpty(b[c])
    requires forall k :: GetDay(a, k) == GetDay(b, k)
    ensures a == b
  {
    DayStore_domain_nonempty(a);
    DayStore_domain_nonempty(b);
    assert forall k :: (k in a) <==> (k in b);
    assert forall k :: k in a ==> a[k] == b[k];
  }

  lemma AdjustDays_stores_nonempty(store: DayStore, lines: seq<LineRow>,
                                   orderkey: int, p: Priority, sign: int)
    requires p.High? || p.Low?
    requires forall c :: c in store ==> CellNonEmpty(store[c])
    ensures forall c :: c in AdjustDaysForOrder(store, lines, orderkey, p, sign) ==>
              CellNonEmpty(AdjustDaysForOrder(store, lines, orderkey, p, sign)[c])
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      AdjustDays_stores_nonempty(store, pref, orderkey, p, sign);
      var mid := AdjustDaysForOrder(store, pref, orderkey, p, sign);
      var l := lines[|lines| - 1];
      if LineQualifies(l) && l.orderkey == orderkey {
        var ck := CellKey(l.receiptdate, l.shipmode);
        var s2 := ApplyContribution(GetDay(mid, ck), p, sign);
        assert AdjustDaysForOrder(store, lines, orderkey, p, sign) == WriteDay(mid, ck, s2);
        if s2.high == 0 && s2.low == 0 {
          assert WriteDay(mid, ck, s2) == mid - {ck};
        } else {
          assert CellNonEmpty(s2);
          assert WriteDay(mid, ck, s2) == mid[ck := s2];
        }
      }
    }
  }

  lemma AdjustDays_wellformed_plus(store: DayStore, lines: seq<LineRow>, orderkey: int, p: Priority)
    requires p.High? || p.Low?
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures forall c :: c in AdjustDaysForOrder(store, lines, orderkey, p, 1) ==>
              CellNonEmpty(AdjustDaysForOrder(store, lines, orderkey, p, 1)[c]) &&
              StatsNonNeg(AdjustDaysForOrder(store, lines, orderkey, p, 1)[c])
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      var pref := lines[..|lines| - 1];
      AdjustDays_wellformed_plus(store, pref, orderkey, p);
      var mid := AdjustDaysForOrder(store, pref, orderkey, p, 1);
      var l := lines[|lines| - 1];
      if LineQualifies(l) && l.orderkey == orderkey {
        var ck := CellKey(l.receiptdate, l.shipmode);
        var s0 := GetDay(mid, ck);
        assert StatsNonNeg(s0) by {
          if ck in mid {
          } else {
            assert s0 == EmptyStats();
          }
        }
        ApplyContribution_plus1_nonempty(s0, p);
      }
    }
  }

  lemma FoldLines_insertPriority(store: DayStore, pri: PriorityStore,
                                  lines: seq<LineRow>, orderkey: int, p: Priority)
    requires orderkey !in pri
    requires p.High? || p.Low?
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures FoldLines(store, pri[orderkey := p], lines)
         == AdjustDaysForOrder(FoldLines(store, pri, lines), lines, orderkey, p, 1)
  {
    var pri2 := pri[orderkey := p];
    var base := FoldLines(store, pri, lines);
    FoldLines_all_nonempty(store, pri, lines);
    FoldLines_all_nonempty(store, pri2, lines);
    AdjustDays_wellformed_plus(base, lines, orderkey, p);
    forall k
      ensures GetDay(FoldLines(store, pri2, lines), k)
           == GetDay(AdjustDaysForOrder(base, lines, orderkey, p, 1), k)
    {
      FoldLines_get(store, pri, lines, k);
      FoldLines_get(store, pri2, lines, k);
      LinesCellContrib_insert(pri, lines, orderkey, p, k);
      AdjustDays_get(base, lines, orderkey, p, 1, k);
    }
    DayStore_eq_from_get(FoldLines(store, pri2, lines),
                         AdjustDaysForOrder(base, lines, orderkey, p, 1));
  }

  lemma FoldLines_removePriority(store: DayStore, pri: PriorityStore,
                                  lines: seq<LineRow>, orderkey: int, p: Priority)
    requires orderkey in pri && pri[orderkey] == p
    requires p.High? || p.Low?
    requires forall c :: c in store ==> CellNonEmpty(store[c]) && StatsNonNeg(store[c])
    ensures FoldLines(store, pri - {orderkey}, lines)
         == AdjustDaysForOrder(FoldLines(store, pri, lines), lines, orderkey, p, -1)
  {
    var pri2 := pri - {orderkey};
    var base := FoldLines(store, pri, lines);
    FoldLines_all_nonempty(store, pri, lines);
    FoldLines_all_nonempty(store, pri2, lines);
    AdjustDays_stores_nonempty(base, lines, orderkey, p, -1);
    forall k
      ensures GetDay(FoldLines(store, pri2, lines), k)
           == GetDay(AdjustDaysForOrder(base, lines, orderkey, p, -1), k)
    {
      FoldLines_get(store, pri, lines, k);
      FoldLines_get(store, pri2, lines, k);
      LinesCellContrib_remove(pri, lines, orderkey, p, k);
      AdjustDays_get(base, lines, orderkey, p, -1, k);
    }
    DayStore_eq_from_get(FoldLines(store, pri2, lines),
                         AdjustDaysForOrder(base, lines, orderkey, p, -1));
  }

  lemma AdjustDays_Absent_noop(store: DayStore, lines: seq<LineRow>, orderkey: int, sign: int)
    ensures AdjustDaysForOrder(store, lines, orderkey, Absent, sign) == store
    decreases |lines|
  {
    if |lines| == 0 {
    } else {
      AdjustDays_Absent_noop(store, lines[..|lines| - 1], orderkey, sign);
    }
  }

  lemma AdjustDays_cancel(store: DayStore, lines: seq<LineRow>, orderkey: int, p: Priority)
    requires p.High? || p.Low?
    requires forall c :: c in store ==> CellNonEmpty(store[c])
    ensures AdjustDaysForOrder(AdjustDaysForOrder(store, lines, orderkey, p, -1),
                               lines, orderkey, p, 1) == store
  {
    var mid := AdjustDaysForOrder(store, lines, orderkey, p, -1);
    AdjustDays_stores_nonempty(store, lines, orderkey, p, -1);
    AdjustDays_stores_nonempty(mid, lines, orderkey, p, 1);
    forall k
      ensures GetDay(AdjustDaysForOrder(mid, lines, orderkey, p, 1), k) == GetDay(store, k)
    {
      AdjustDays_get(store, lines, orderkey, p, -1, k);
      AdjustDays_get(mid, lines, orderkey, p, 1, k);
    }
    DayStore_eq_from_get(AdjustDaysForOrder(mid, lines, orderkey, p, 1), store);
  }

  lemma ReclassifyOrder_eq_BuildFold_insert(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, o: OrderRow)
    requires Inv(s, orders, lines)
    requires forall i :: 0 <= i < |orders| ==> orders[i].orderkey != o.orderkey
    ensures ReclassifyOrder(s, lines, o.orderkey, ClassifyPriority(o.priority))
         == BuildFold(orders + [o], lines)
  {
    var p := ClassifyPriority(o.priority);
    FoldPriorities_fresh(orders, o);
    FoldPriorities_snoc(orders, o);
    assert o.orderkey !in s.priorities;
    assert GetPriority(s.priorities, o.orderkey) == Absent;
    assert p.High? || p.Low?;
    AdjustDays_Absent_noop(s.days, lines, o.orderkey, -1);
    assert s.days == FoldLines(map[], s.priorities, lines);
    FoldLines_insertPriority(map[], s.priorities, lines, o.orderkey, p);
    var s2 := ReclassifyOrder(s, lines, o.orderkey, p);
    assert s2.priorities == s.priorities[o.orderkey := p];
    assert s2.days == AdjustDaysForOrder(s.days, lines, o.orderkey, p, 1);
    assert s2 == BuildFold(orders + [o], lines);
  }

  lemma {:isolate_assertions} ReclassifyOrder_eq_BuildFold_delete(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, o: OrderRow)
    requires Inv(s, orders, lines)
    requires |orders| > 0 && orders[|orders| - 1] == o
    requires forall i :: 0 <= i < |orders| - 1 ==> orders[i].orderkey != o.orderkey
    ensures ReclassifyOrder(s, lines, o.orderkey, Absent)
         == BuildFold(orders[..|orders| - 1], lines)
  {
    var p := ClassifyPriority(o.priority);
    var ok := o.orderkey;
    FoldPriorities_unsoc(orders, o);
    FoldPriorities_fresh(orders[..|orders| - 1], o);
    assert ok in s.priorities && s.priorities[ok] == p;
    assert GetPriority(s.priorities, ok) == p;
    assert p.High? || p.Low?;
    assert s.days == FoldLines(map[], s.priorities, lines);
    FoldLines_removePriority(map[], s.priorities, lines, ok, p);
    var d1 := AdjustDaysForOrder(s.days, lines, ok, p, -1);
    AdjustDays_Absent_noop(d1, lines, ok, 1);
    assert AdjustDaysForOrder(d1, lines, ok, Absent, 1) == d1;
    var s2 := ReclassifyOrder(s, lines, ok, Absent);
    assert s2 == State(s.priorities - {ok}, d1);
    assert FoldPriorities(orders[..|orders| - 1]) == s.priorities - {ok};
    assert FoldLines(map[], s.priorities - {ok}, lines) == d1;
    assert BuildFold(orders[..|orders| - 1], lines)
        == State(s.priorities - {ok}, d1);
  }

  lemma InvPreservedByOrderInsert(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, o: OrderRow)
    requires Inv(s, orders, lines)
    requires forall i :: 0 <= i < |orders| ==> orders[i].orderkey != o.orderkey
    ensures Inv(ReclassifyOrder(s, lines, o.orderkey, ClassifyPriority(o.priority)),
                orders + [o], lines)
  {
    ReclassifyOrder_eq_BuildFold_insert(s, orders, lines, o);
  }

  lemma InvPreservedByOrderDeleteLast(s: State, orders: seq<OrderRow>, lines: seq<LineRow>, o: OrderRow)
    requires Inv(s, orders, lines)
    requires |orders| > 0 && orders[|orders| - 1] == o
    requires forall i :: 0 <= i < |orders| - 1 ==> orders[i].orderkey != o.orderkey
    ensures Inv(ReclassifyOrder(s, lines, o.orderkey, Absent),
                orders[..|orders| - 1], lines)
  {
    ReclassifyOrder_eq_BuildFold_delete(s, orders, lines, o);
  }

  // Reclassify to pNew equals delete-to-Absent then insert-pNew (priority edit).
  lemma {:isolate_assertions} ReclassifyOrder_update_via_Absent(s: State, lines: seq<LineRow>,
                                                                ok: int, pOld: Priority, pNew: Priority)
    requires GetPriority(s.priorities, ok) == pOld
    requires pOld.High? || pOld.Low?
    requires pNew.High? || pNew.Low?
    requires forall c :: c in s.days ==> CellNonEmpty(s.days[c])
    ensures ReclassifyOrder(s, lines, ok, pNew)
         == ReclassifyOrder(ReclassifyOrder(s, lines, ok, Absent), lines, ok, pNew)
  {
    var mid := ReclassifyOrder(s, lines, ok, Absent);
    var d1 := AdjustDaysForOrder(s.days, lines, ok, pOld, -1);
    AdjustDays_Absent_noop(d1, lines, ok, 1);
    assert mid == State(s.priorities - {ok}, d1);
    AdjustDays_Absent_noop(mid.days, lines, ok, -1);
    if pOld == pNew {
      AdjustDays_cancel(s.days, lines, ok, pOld);
      assert ReclassifyOrder(s, lines, ok, pNew)
          == State(SetPriority(s.priorities, ok, pNew), s.days);
      assert ReclassifyOrder(mid, lines, ok, pNew)
          == State(mid.priorities[ok := pNew],
                   AdjustDaysForOrder(mid.days, lines, ok, pNew, 1));
      assert mid.priorities[ok := pNew] == s.priorities[ok := pNew];
      assert SetPriority(s.priorities, ok, pNew) == s.priorities[ok := pNew];
      assert AdjustDaysForOrder(mid.days, lines, ok, pNew, 1) == s.days;
    } else {
      assert ReclassifyOrder(s, lines, ok, pNew)
          == State(s.priorities[ok := pNew],
                   AdjustDaysForOrder(d1, lines, ok, pNew, 1));
      assert ReclassifyOrder(mid, lines, ok, pNew)
          == State(mid.priorities[ok := pNew],
                   AdjustDaysForOrder(mid.days, lines, ok, pNew, 1));
      assert mid.priorities[ok := pNew] == s.priorities[ok := pNew];
      assert mid.days == d1;
    }
  }

  lemma InvPreservedByOrderUpdate(s: State, orders: seq<OrderRow>, lines: seq<LineRow>,
                                  oOld: OrderRow, oNew: OrderRow)
    requires Inv(s, orders, lines)
    requires |orders| > 0 && orders[|orders| - 1] == oOld
    requires oOld.orderkey == oNew.orderkey
    requires forall i :: 0 <= i < |orders| - 1 ==> orders[i].orderkey != oOld.orderkey
    ensures Inv(ReclassifyOrder(s, lines, oNew.orderkey, ClassifyPriority(oNew.priority)),
                orders[..|orders| - 1] + [oNew], lines)
  {
    var ok := oOld.orderkey;
    var pOld := ClassifyPriority(oOld.priority);
    var pNew := ClassifyPriority(oNew.priority);
    var mid := ReclassifyOrder(s, lines, ok, Absent);
    InvPreservedByOrderDeleteLast(s, orders, lines, oOld);
    InvPreservedByOrderInsert(mid, orders[..|orders| - 1], lines, oNew);
    assert GetPriority(s.priorities, ok) == pOld;
    assert pOld.High? || pOld.Low?;
    assert pNew.High? || pNew.Low?;
    assert forall c :: c in s.days ==> CellNonEmpty(s.days[c]) by {
      assert s.days == FoldLines(map[], s.priorities, lines);
      FoldLines_all_nonempty(map[], s.priorities, lines);
    }
    ReclassifyOrder_update_via_Absent(s, lines, ok, pOld, pNew);
    assert ReclassifyOrder(s, lines, ok, pNew)
        == ReclassifyOrder(mid, lines, ok, pNew);
  }
}
