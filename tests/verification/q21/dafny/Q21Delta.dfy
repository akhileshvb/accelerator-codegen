// Database semantics of the transaction deltas captured by AccumulateMutation.
//
// A transaction's Q21-relevant writes (no supplier / nation writes: those
// force a rebuild) are the lineitem delta sequence LD and the order delta
// sequence OD, each in capture order.  The committed database is
//   T' = TDB(T.sup, ApplyLi(T.li, LD), ApplyOrd(T.ords, OD), T.nats)
// provided every delta is valid in the intermediate state (deletes / updates
// refer to an existing row; inserts respect the orders primary key).
//
// Lemmas:
//   Summary_after : SummaryOf(ApplyLi(L, LD), ok) == ApplyLiDeltas(SummaryOf(L, ok), DeltasFor(LD, ok))
//   Status_after  : StatusOfSeq(ApplyOrd(O, OD), ok) == FinalStatus(...) (last change wins)
//   Insert_first  : FirstOldStatus(OD, ok) == 0 ==> ok absent from O

include "Q21Refine.dfy"

module Q21Delta {
  import opened Q21Types
  import opened Q21ModuleTwin
  import opened Q21Contrib
  import opened Q21Count
  import opened Q21Refine

  function LiOf(d: LineitemDelta): Li { Li(d.orderkey, d.suppkey, d.commitdate, d.receiptdate) }

  function RemoveOne(L: seq<Li>, x: Li): seq<Li>
  { if |L| == 0 then [] else if L[0] == x then L[1..] else [L[0]] + RemoveOne(L[1..], x) }

  function LiStep(L: seq<Li>, d: LineitemDelta): seq<Li>
  { if d.sign > 0 then L + [LiOf(d)] else RemoveOne(L, LiOf(d)) }

  function ApplyLi(L: seq<Li>, LD: seq<LineitemDelta>): seq<Li>
    decreases |LD|
  { if |LD| == 0 then L else ApplyLi(LiStep(L, LD[0]), LD[1..]) }

  ghost predicate LiValid(L: seq<Li>, LD: seq<LineitemDelta>)
    decreases |LD|
  {
    |LD| == 0 ||
    ((LD[0].sign == 1 || (LD[0].sign == -1 && LiOf(LD[0]) in L)) && LiValid(LiStep(L, LD[0]), LD[1..]))
  }

  function RemoveOrd(O: seq<Ord>, ok: int): seq<Ord>
  { if |O| == 0 then [] else if O[0].ok == ok then RemoveOrd(O[1..], ok) else [O[0]] + RemoveOrd(O[1..], ok) }

  function SetStatus(O: seq<Ord>, ok: int, st: char): seq<Ord>
  { if |O| == 0 then [] else (if O[0].ok == ok then [Ord(ok, st)] else [O[0]]) + SetStatus(O[1..], ok, st) }

  function OrdStep(O: seq<Ord>, d: OrderDelta): seq<Ord>
  {
    if d.old_status == 0 as char then O + [Ord(d.orderkey, d.new_status)]
    else if d.new_status == 0 as char then RemoveOrd(O, d.orderkey)
    else SetStatus(O, d.orderkey, d.new_status)
  }

  function ApplyOrd(O: seq<Ord>, OD: seq<OrderDelta>): seq<Ord>
    decreases |OD|
  { if |OD| == 0 then O else ApplyOrd(OrdStep(O, OD[0]), OD[1..]) }

  ghost predicate OrdKeysUniq(O: seq<Ord>) { forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok }
  ghost predicate StatusNZ(O: seq<Ord>) { forall o :: o in O ==> o.st != 0 as char }

  ghost predicate OrdValid(O: seq<Ord>, OD: seq<OrderDelta>)
    decreases |OD|
  {
    |OD| == 0 ||
    (var d := OD[0];
     (if d.old_status == 0 as char then d.new_status != 0 as char && (forall o :: o in O ==> o.ok != d.orderkey)
      else exists o :: o in O && o.ok == d.orderkey && o.st == d.old_status) &&
     OrdValid(OrdStep(O, d), OD[1..]))
  }

  // ===================== Lineitems =====================

  lemma SumF_remove(L: seq<Li>, x: Li, f: Li -> int)
    requires x in L
    ensures SumF(RemoveOne(L, x), f) == SumF(L, f) - f(x)
  {
    if L[0] != x {
      assert x in L[1..];
      SumF_remove(L[1..], x, f);
    }
  }

  lemma SumF_append(L: seq<Li>, x: Li, f: Li -> int)
    ensures SumF(L + [x], f) == SumF(L, f) + f(x)
  {
    SumF_app(L, [x], f);
  }

  lemma RemoveOne_mem(L: seq<Li>, x: Li, y: Li)
    ensures y in RemoveOne(L, x) ==> y in L
  {
    if |L| > 0 && L[0] != x { RemoveOne_mem(L[1..], x, y); }
  }

  function Delta1(d: LineitemDelta, ok: int, sk: int): SOI
  {
    if d.orderkey == ok && d.suppkey == sk
    then SOI(d.sign, if d.receiptdate > d.commitdate then d.sign else 0) else SOI(0, 0)
  }

  ghost function TotF(ok: int, sk: int): Li -> int
  { (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0 }
  ghost function LateF(ok: int, sk: int): Li -> int
  { (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0 }

  lemma Tot_step(L: seq<Li>, d: LineitemDelta, ok: int, sk: int)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    ensures SumF(LiStep(L, d), TotF(ok, sk)) == SumF(L, TotF(ok, sk)) + Delta1(d, ok, sk).total
  {
    var x := LiOf(d);
    var f := TotF(ok, sk);
    assert f(x) == (if d.orderkey == ok && d.suppkey == sk then 1 else 0);
    if d.sign > 0 { SumF_append(L, x, f); } else { SumF_remove(L, x, f); }
  }

  lemma Late_step(L: seq<Li>, d: LineitemDelta, ok: int, sk: int)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    ensures SumF(LiStep(L, d), LateF(ok, sk)) == SumF(L, LateF(ok, sk)) + Delta1(d, ok, sk).late
  {
    var x := LiOf(d);
    var f := LateF(ok, sk);
    assert f(x) == (if d.orderkey == ok && d.suppkey == sk && d.receiptdate > d.commitdate then 1 else 0);
    if d.sign > 0 { SumF_append(L, x, f); } else { SumF_remove(L, x, f); }
  }

  lemma Summary_step_point(L: seq<Li>, d: LineitemDelta, ok: int, sk: int)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    ensures GetSOI(SummaryOf(LiStep(L, d), ok), sk) == AddSOI2(GetSOI(SummaryOf(L, ok), sk), Delta1(d, ok, sk))
  {
    Tot_step(L, d, ok, sk);
    Late_step(L, d, ok, sk);
    Summary_get(L, ok, sk);
    Summary_get(LiStep(L, d), ok, sk);
  }

  lemma ApplyOne_point(sup: map<int, SOI>, d: LineitemDelta, sk: int)
    requires var v := AddSOI2(GetSOI(sup, d.suppkey), Delta1(d, d.orderkey, d.suppkey));
             v.total >= 0 && v.late >= 0 && v.late <= v.total
    ensures GetSOI(ApplyLiDeltas(sup, [d]), sk) == AddSOI2(GetSOI(sup, sk), Delta1(d, d.orderkey, sk))
    ensures sk in ApplyLiDeltas(sup, [d]) ==> ((sk in sup && sk != d.suppkey) ||
            (sk == d.suppkey && GetSOI(ApplyLiDeltas(sup, [d]), sk).total > 0))
  {
    assert [d][1..] == [];
  }

  lemma {:isolate_assertions} Summary_step_other(L: seq<Li>, d: LineitemDelta, ok: int)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    requires d.orderkey != ok
    ensures SummaryOf(LiStep(L, d), ok) == SummaryOf(L, ok)
  {
    var L2 := LiStep(L, d);
    var S := SummaryOf(L, ok);
    forall sk ensures GetSOI(S, sk) == GetSOI(SummaryOf(L2, ok), sk) {
      Summary_step_point(L, d, ok, sk);
      assert Delta1(d, ok, sk) == SOI(0, 0);
      var g := GetSOI(S, sk);
      assert AddSOI2(g, SOI(0, 0)) == g;
    }
    forall sk | sk in S ensures S[sk].total > 0 {
      SummaryOf_late_le(L, ok, sk);
    }
    Summary_ext(S, L2, ok);
  }

  lemma {:isolate_assertions} Summary_step_same(L: seq<Li>, d: LineitemDelta)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    ensures SummaryOf(LiStep(L, d), d.orderkey) == ApplyLiDeltas(SummaryOf(L, d.orderkey), [d])
  {
    var ok := d.orderkey;
    var L2 := LiStep(L, d);
    var S := SummaryOf(L, ok);
    var W := ApplyLiDeltas(S, [d]);
    Summary_step_point(L, d, ok, d.suppkey);
    SummaryOf_late_le(L2, ok, d.suppkey);
    SummaryOf_late_le(L, ok, d.suppkey);
    forall sk ensures GetSOI(W, sk) == GetSOI(SummaryOf(L2, ok), sk) {
      Summary_step_point(L, d, ok, sk);
      ApplyOne_point(S, d, sk);
    }
    forall sk | sk in W ensures W[sk].total > 0 {
      SummaryOf_late_le(L, ok, sk);
      ApplyOne_point(S, d, sk);
    }
    Summary_ext(W, L2, ok);
  }

  lemma Summary_step(L: seq<Li>, d: LineitemDelta, ok: int)
    requires d.sign == 1 || (d.sign == -1 && LiOf(d) in L)
    ensures SummaryOf(LiStep(L, d), ok) ==
            (if d.orderkey == ok then ApplyLiDeltas(SummaryOf(L, ok), [d]) else SummaryOf(L, ok))
  {
    if d.orderkey == ok { Summary_step_same(L, d); } else { Summary_step_other(L, d, ok); }
  }

  function AddSOI2(a: SOI, b: SOI): SOI { SOI(a.total + b.total, a.late + b.late) }

  lemma Summary_get(L: seq<Li>, ok: int, sk: int)
    ensures GetSOI(SummaryOf(L, ok), sk) == SOI(SumF(L, TotF(ok, sk)), SumF(L, LateF(ok, sk)))
  {
    reveal SummaryOf();
    SummaryOf_late_le(L, ok, sk);
    SumF_ext(L, TotF(ok, sk), (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
    SumF_ext(L, LateF(ok, sk), (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
    if sk !in SummaryOf(L, ok) {
      forall l | l in L ensures !(l.ok == ok && l.sk == sk) {
        if l.ok == ok && l.sk == sk { assert sk in SupsOf(L, ok); }
      }
      SumF_zero(L, TotF(ok, sk));
      SumF_zero(L, LateF(ok, sk));
    }
  }

  lemma ApplyLiDeltas_cons(sup: map<int, SOI>, d: LineitemDelta, ds: seq<LineitemDelta>)
    ensures ApplyLiDeltas(sup, [d] + ds) == ApplyLiDeltas(ApplyLiDeltas(sup, [d]), ds)
  {
    assert ([d] + ds)[1..] == ds;
    assert [d][1..] == [];
  }

  lemma Summary_after(L: seq<Li>, LD: seq<LineitemDelta>, ok: int)
    requires LiValid(L, LD)
    ensures SummaryOf(ApplyLi(L, LD), ok) == ApplyLiDeltas(SummaryOf(L, ok), DeltasFor(LD, ok))
    decreases |LD|
  {
    if |LD| > 0 {
      var d := LD[0];
      Summary_step(L, d, ok);
      Summary_after(LiStep(L, d), LD[1..], ok);
      if d.orderkey == ok {
        assert DeltasFor(LD, ok) == [d] + DeltasFor(LD[1..], ok);
        ApplyLiDeltas_cons(SummaryOf(L, ok), d, DeltasFor(LD[1..], ok));
      } else {
        assert DeltasFor(LD, ok) == DeltasFor(LD[1..], ok);
      }
    }
  }

  lemma DeltasFor_empty(LD: seq<LineitemDelta>, ok: int)
    requires forall d :: d in LD ==> d.orderkey != ok
    ensures DeltasFor(LD, ok) == []
  {
    if |LD| > 0 { DeltasFor_empty(LD[1..], ok); }
  }

  // Keys of the updated per-order summary: old keys plus delta suppliers.
  lemma ApplyLiDeltas_keys(sup: map<int, SOI>, ds: seq<LineitemDelta>)
    ensures forall sk :: sk in ApplyLiDeltas(sup, ds) ==> sk in sup || exists d :: d in ds && d.suppkey == sk
    decreases |ds|
  {
    if |ds| > 0 {
      var d := ds[0];
      var info := if d.suppkey in sup then sup[d.suppkey] else SOI(0, 0);
      var info2 := SOI(info.total + d.sign, if d.receiptdate > d.commitdate then info.late + d.sign else info.late);
      var sup2 := if info2.total <= 0 && info2.late <= 0 then sup - {d.suppkey} else sup[d.suppkey := info2];
      ApplyLiDeltas_keys(sup2, ds[1..]);
      assert forall x :: x in ds <==> (x == ds[0] || x in ds[1..]);
    }
  }

  lemma DeltasFor_mem(LD: seq<LineitemDelta>, ok: int, d: LineitemDelta)
    ensures d in DeltasFor(LD, ok) ==> d in LD && d.orderkey == ok
  {
    if |LD| > 0 { DeltasFor_mem(LD[1..], ok, d); }
  }

  // ===================== Orders =====================

  lemma RemoveOrd_status(O: seq<Ord>, ok: int, k: int)
    ensures StatusOfSeq(RemoveOrd(O, ok), k) == (if k == ok then 0 as char else StatusOfSeq(O, k))
    ensures forall o :: o in RemoveOrd(O, ok) ==> o in O && o.ok != ok
  {
    if |O| > 0 { RemoveOrd_status(O[1..], ok, k); }
  }

  lemma SetStatus_status(O: seq<Ord>, ok: int, st: char, k: int)
    requires exists o :: o in O && o.ok == ok
    ensures StatusOfSeq(SetStatus(O, ok, st), k) == (if k == ok then st else StatusOfSeq(O, k))
  {
    SetStatus_status_gen(O, ok, st, k);
  }

  lemma SetStatus_status_gen(O: seq<Ord>, ok: int, st: char, k: int)
    ensures StatusOfSeq(SetStatus(O, ok, st), k) ==
            (if k == ok then (if exists o :: o in O && o.ok == ok then st else 0 as char) else StatusOfSeq(O, k))
  {
    if |O| > 0 {
      SetStatus_status_gen(O[1..], ok, st, k);
      assert forall x :: x in O <==> (x == O[0] || x in O[1..]);
      if k == ok && !(exists o :: o in O[1..] && o.ok == ok) && O[0].ok != ok {
        StatusOf_absent_seq(O[1..], ok);
      }
    }
  }

  lemma SetStatus_mem(O: seq<Ord>, ok: int, st: char)
    ensures forall o :: o in SetStatus(O, ok, st) ==> (o in O && o.ok != ok) || o == Ord(ok, st)
    ensures forall o :: o in O && o.ok != ok ==> o in SetStatus(O, ok, st)
    ensures (exists o :: o in O && o.ok == ok) ==> Ord(ok, st) in SetStatus(O, ok, st)
  {
    if |O| > 0 { SetStatus_mem(O[1..], ok, st); assert forall x :: x in O <==> (x == O[0] || x in O[1..]); }
  }

  lemma StatusOf_absent_seq(O: seq<Ord>, ok: int)
    requires forall o :: o in O ==> o.ok != ok
    ensures StatusOfSeq(O, ok) == 0 as char
  { if |O| > 0 { StatusOf_absent_seq(O[1..], ok); } }

  lemma StatusOf_app(O: seq<Ord>, x: Ord, k: int)
    requires forall o :: o in O ==> o.ok != x.ok
    ensures StatusOfSeq(O + [x], k) == (if k == x.ok then x.st else StatusOfSeq(O, k))
  {
    if |O| == 0 { assert O + [x] == [x]; }
    else {
      assert (O + [x])[1..] == O[1..] + [x];
      StatusOf_app(O[1..], x, k);
      if O[0].ok == k && k == x.ok { assert O[0] in O; }
    }
  }

  lemma StatusOf_present(O: seq<Ord>, o: Ord)
    requires OrdKeysUniq(O) && o in O
    ensures StatusOfSeq(O, o.ok) == o.st
  {
    if O[0] != o {
      if O[0].ok == o.ok { var k :| 0 <= k < |O| && O[k] == o; assert k > 0 && O[0].ok != O[k].ok; }
      assert o in O[1..];
      assert OrdKeysUniq(O[1..]) by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      StatusOf_present(O[1..], o);
    }
  }

  // Status after one valid order delta (under a unique-key orders table).
  lemma OrdStep_status(O: seq<Ord>, d: OrderDelta, k: int)
    requires OrdKeysUniq(O)
    requires d.old_status == 0 as char ==> forall o :: o in O ==> o.ok != d.orderkey
    requires d.old_status != 0 as char ==> exists o :: o in O && o.ok == d.orderkey && o.st == d.old_status
    ensures StatusOfSeq(OrdStep(O, d), k) == (if k == d.orderkey then d.new_status else StatusOfSeq(O, k))
  {
    if d.old_status == 0 as char {
      StatusOf_app(O, Ord(d.orderkey, d.new_status), k);
    } else if d.new_status == 0 as char {
      RemoveOrd_status(O, d.orderkey, k);
    } else {
      SetStatus_status(O, d.orderkey, d.new_status, k);
    }
  }

  lemma OrdStep_uniq(O: seq<Ord>, d: OrderDelta)
    requires OrdKeysUniq(O)
    requires d.old_status == 0 as char ==> forall o :: o in O ==> o.ok != d.orderkey
    ensures OrdKeysUniq(OrdStep(O, d))
  {
    if d.old_status == 0 as char {
      var O2 := O + [Ord(d.orderkey, d.new_status)];
      forall i, j | 0 <= i < j < |O2| ensures O2[i].ok != O2[j].ok {
        if j == |O| { assert O2[i] in O; }
      }
    } else if d.new_status == 0 as char {
      RemoveOrd_uniq(O, d.orderkey);
    } else {
      SetStatus_uniq(O, d.orderkey, d.new_status);
    }
  }

  lemma RemoveOrd_uniq(O: seq<Ord>, ok: int)
    requires OrdKeysUniq(O)
    ensures OrdKeysUniq(RemoveOrd(O, ok))
  {
    if |O| > 0 {
      assert OrdKeysUniq(O[1..]) by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      RemoveOrd_uniq(O[1..], ok);
      if O[0].ok != ok {
        var R := RemoveOrd(O[1..], ok);
        RemoveOrd_status(O[1..], ok, 0);
        forall o | o in R ensures o.ok != O[0].ok {
          assert o in O[1..];
          var k :| 0 <= k < |O[1..]| && O[1..][k] == o; assert O[0].ok != O[k + 1].ok;
        }
        var R2 := [O[0]] + R;
        forall i, j | 0 <= i < j < |R2| ensures R2[i].ok != R2[j].ok { if i == 0 { assert R2[j] in R; } }
      }
    }
  }

  lemma SetStatus_uniq(O: seq<Ord>, ok: int, st: char)
    requires OrdKeysUniq(O)
    ensures OrdKeysUniq(SetStatus(O, ok, st))
    ensures |SetStatus(O, ok, st)| == |O|
    ensures forall i :: 0 <= i < |O| ==> SetStatus(O, ok, st)[i].ok == O[i].ok
  {
    if |O| > 0 {
      assert OrdKeysUniq(O[1..]) by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      SetStatus_uniq(O[1..], ok, st);
    }
  }

  // Final status after a valid delta sequence: the new status of the last
  // status-changing delta, else the original status.
  lemma Status_after(O: seq<Ord>, OD: seq<OrderDelta>, ok: int)
    requires OrdKeysUniq(O) && OrdValid(O, OD)
    ensures StatusOfSeq(ApplyOrd(O, OD), ok) ==
            (if StatusChange(OD, ok).Change? then StatusChange(OD, ok).new_status else StatusOfSeq(O, ok))
    ensures OrdKeysUniq(ApplyOrd(O, OD))
    decreases |OD|
  {
    if |OD| > 0 {
      var d := OD[0];
      OrdStep_status(O, d, ok);
      OrdStep_uniq(O, d);
      Status_after(OrdStep(O, d), OD[1..], ok);
      if d.orderkey == ok && d.old_status == d.new_status && !StatusChange(OD[1..], ok).Change? {
        // A no-op status delta: old status is the current status.
        if d.old_status == 0 as char {
          assert d.new_status != 0 as char;
        } else {
          var o :| o in O && o.ok == ok && o.st == d.old_status;
          StatusOf_present(O, o);
        }
      }
    }
  }

  lemma Insert_first(O: seq<Ord>, OD: seq<OrderDelta>, ok: int)
    requires OrdValid(O, OD)
    requires FirstOldStatus(OD, ok) == SomeChar(0 as char)
    ensures forall o :: o in O ==> o.ok != ok
    decreases |OD|
  {
    var d := OD[0];
    if d.orderkey != ok {
      Insert_first(OrdStep(O, d), OD[1..], ok);
      forall o | o in O ensures o.ok != ok {
        if d.old_status == 0 as char {
          assert o in OrdStep(O, d);
        } else if d.new_status == 0 as char {
          if o.ok != d.orderkey { RemoveOrd_keep(O, d.orderkey, o); }
        } else {
          SetStatus_mem(O, d.orderkey, d.new_status);
        }
      }
    }
  }

  lemma RemoveOrd_keep(O: seq<Ord>, ok: int, o: Ord)
    requires o in O && o.ok != ok
    ensures o in RemoveOrd(O, ok)
  {
    if O[0] != o { assert o in O[1..]; RemoveOrd_keep(O[1..], ok, o); }
  }

  // Orders not mentioned by any delta keep their status and row.
  lemma Unaffected_status(O: seq<Ord>, OD: seq<OrderDelta>, ok: int)
    requires OrdKeysUniq(O) && OrdValid(O, OD)
    requires forall d :: d in OD ==> d.orderkey != ok
    ensures StatusOfSeq(ApplyOrd(O, OD), ok) == StatusOfSeq(O, ok)
  {
    Status_after(O, OD, ok);
    NoChange_of(OD, ok);
  }

  lemma NoChange_of(OD: seq<OrderDelta>, ok: int)
    requires forall d :: d in OD ==> d.orderkey != ok
    ensures StatusChange(OD, ok) == NoChange
  {
    if |OD| > 0 { NoChange_of(OD[1..], ok); }
  }

  lemma Keys_after(O: seq<Ord>, OD: seq<OrderDelta>, k: int)
    requires OrdKeysUniq(O) && OrdValid(O, OD) && StatusNZ(O)
    ensures StatusNZ(ApplyOrd(O, OD))
    ensures (exists o :: o in ApplyOrd(O, OD) && o.ok == k) ==>
            (exists o :: o in O && o.ok == k) || exists d :: d in OD && d.orderkey == k
    decreases |OD|
  {
    if |OD| > 0 {
      var d := OD[0];
      var O2 := OrdStep(O, d);
      OrdStep_uniq(O, d);
      assert StatusNZ(O2) by {
        if d.old_status == 0 as char {
        } else if d.new_status == 0 as char {
          RemoveOrd_status(O, d.orderkey, 0);
        } else {
          SetStatus_mem(O, d.orderkey, d.new_status);
        }
      }
      Keys_after(O2, OD[1..], k);
      if exists o :: o in O2 && o.ok == k {
        var o :| o in O2 && o.ok == k;
        if d.old_status == 0 as char {
          if o !in O { assert o == Ord(d.orderkey, d.new_status); }
        } else if d.new_status == 0 as char {
          RemoveOrd_status(O, d.orderkey, 0);
        } else {
          SetStatus_mem(O, d.orderkey, d.new_status);
        }
      }
      assert forall x :: x in OD <==> (x == OD[0] || x in OD[1..]);
    }
  }
}
