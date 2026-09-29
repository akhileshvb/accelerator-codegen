// Inv_Q21 (StoreInv) and execution correctness of the Twin Execute.
//
// StoreInv(snap, T): every published Q21 partition image in the (fully valid)
// store snapshot represents the typed database T:
//   p0 SupplierMetadata : supplier_names / supplier_nations == T's supplier maps
//   p1 OrderSummaries   : every summarized order is exact (status, per-supplier
//                         total/late counts); every unsummarized order
//                         contributes nothing (Cv == 0)
//   p2 NationNameIndex  : name -> distinct nation keys of that name
//   1000+nk Nation      : state.nation_numwait[nk] == NWmap(T, nk) and the
//                         pre-sorted output_rows answer Q21 for {nk}
//   2000000+h (name)    : image for name nm (stored at its own hash slot) with
//                         exactly one nation nk: its output_rows answer Q21
//                         for NationsNamed(T, nm) == {nk}
//   absent 1000+nk      : NWmap(T, nk) is empty
//
// Execute_correct: StoreInv + KeysOk + DomOk ==> an emitted hit result equals
// SQLRows(T, nn), the RelNode Eval_Spec result.

include "Q21Answer.dfy"
include "Q21Emit.dfy"

module Q21Inv {
  import opened Q21Types
  import opened Q21Count
  import opened Q21SetSum
  import opened Q21Contrib
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened Q21Answer
  import opened Q21Emit

  ghost function NWmap(T: TDB, nk: int): map<int, int>
  { map sk | sk in SKeys(T) && SupNatOf(T)[sk] == nk && NW(T, sk) != 0 :: NW(T, sk) }

  ghost predicate SupOk(st: TState, T: TDB)
  { st.supplier_names == SupNameOf(T) && st.supplier_nations == SupNatOf(T) }

  ghost predicate NoDupSeq(xs: seq<int>) { forall i, j :: 0 <= i < j < |xs| ==> xs[i] != xs[j] }

  ghost predicate IdxOk(st: TState, T: TDB)
  {
    forall nm ::
      (nm in st.nation_name_to_keys <==> NationsNamed(T, nm) != {}) &&
      (nm in st.nation_name_to_keys ==>
         NoDupSeq(st.nation_name_to_keys[nm]) &&
         (forall x :: x in st.nation_name_to_keys[nm] <==> x in NationsNamed(T, nm)))
  }

  ghost predicate OrdersOk(st: TState, T: TDB)
  {
    forall ok ::
      (FindOrder(st, ok).SomeSummary? ==>
         FindOrder(st, ok).s == OrderSummary(StatusOf(T, ok), SummaryOf(T.li, ok))) &&
      (FindOrder(st, ok).NoSummary? ==> forall sk :: Cv(T, ok, sk) == 0)
  }

  function NwOf(st: TState, nk: int): map<int, int>
  { if nk in st.nation_numwait then st.nation_numwait[nk] else map[] }

  ghost predicate RowsOk(rows: seq<OutRow>, T: TDB, K: set<int>)
  { StrictSorted(rows) && forall x :: x in rows <==> x in AnsSet(T, K) }

  // Partition-id classes (distinct under DomOk; see Q21 layout).
  ghost predicate NkRange(nk: int) { 0 <= nk < kNationNameBase - kNationBase }
  ghost predicate IsNatPid(pid: int) { kNationBase <= pid < kNationNameBase }
  ghost predicate IsNamePid(pid: int) { kNationNameBase <= pid < kNationNameBase + kNationNameSlots }

  ghost predicate ImageOk(pid: int, img: TImage, T: TDB)
  {
    (pid == kSupplierMetadata || pid == kOrderSummaries || pid == kNationNameIndex ||
     IsNatPid(pid) || IsNamePid(pid)) &&
    (pid == kSupplierMetadata ==> img.kind == SupplierMetadata && SupOk(img.state, T)) &&
    (pid == kOrderSummaries ==> img.kind == OrderSummaries && OrdersOk(img.state, T)) &&
    (pid == kNationNameIndex ==> img.kind == NationNameIndex && IdxOk(img.state, T)) &&
    (IsNatPid(pid) ==>
       img.kind == NationK && img.nationkey == pid - kNationBase &&
       NwOf(img.state, pid - kNationBase) == NWmap(T, pid - kNationBase) &&
       RowsOk(img.output_rows, T, {pid - kNationBase})) &&
    (IsNamePid(pid) ==>
       img.kind == NationK && pid == NationNamePartition(img.nation_name) &&
       NationsNamed(T, img.nation_name) == {img.nationkey} &&
       RowsOk(img.output_rows, T, {img.nationkey}))
  }

  ghost predicate StoreInv(snap: map<int, TImage>, T: TDB)
  {
    (forall pid :: pid in snap ==> ImageOk(pid, snap[pid], T)) &&
    (forall nk :: NkRange(nk) && NationPartition(nk) !in snap ==> NWmap(T, nk) == map[])
  }

  // Verified-domain premise: nation keys (of nations and of suppliers) map to
  // distinct, non-overlapping partition ids.
  ghost predicate DomOk(T: TDB)
  {
    (forall s :: s in T.sup ==> NkRange(s.nk)) &&
    (forall n :: n in T.nats ==> NkRange(n.nk))
  }

  // ===================== Sum lemmas =====================

  lemma NW_nonneg(T: TDB, sk: int)
    ensures NW(T, sk) >= 0
  { SumF_nonneg(T.li, (l: Li) => if l.sk == sk && Qual(T, l) then 1 else 0); }

  ghost function VTerm(T: TDB, K: set<int>, nm: string): int -> int
  { (sk: int) => if sk in SKeys(T) && SupNameOf(T)[sk] == nm && SupNatOf(T)[sk] in K then NW(T, sk) else 0 }

  lemma V_single_nation(T: TDB, nk: int, nm: string)
    ensures SetSum(NWmap(T, nk).Keys, RowTerm(NWmap(T, nk), SupNameOf(T), nm)) == V(T, {nk}, nm)
  {
    var m := NWmap(T, nk);
    forall sk | sk in m.Keys ensures RowTerm(m, SupNameOf(T), nm)(sk) == VTerm(T, {nk}, nm)(sk) {
      NW_nonneg(T, sk);
    }
    SetSum_ext(m.Keys, RowTerm(m, SupNameOf(T), nm), VTerm(T, {nk}, nm));
    SetSum_superset(m.Keys, SKeys(T), VTerm(T, {nk}, nm));
  }

  lemma V_union(T: TDB, A: set<int>, B: set<int>, nm: string)
    requires A !! B
    ensures V(T, A + B, nm) == V(T, A, nm) + V(T, B, nm)
  {
    var f := VTerm(T, A, nm);
    var g := VTerm(T, B, nm);
    forall sk | sk in SKeys(T) ensures VTerm(T, A + B, nm)(sk) == f(sk) + g(sk) {
      if SupNatOf(T)[sk] in A { assert SupNatOf(T)[sk] !in B; }
    }
    SetSum_ext(SKeys(T), VTerm(T, A + B, nm), (sk: int) => f(sk) + g(sk));
    SetSum_plus(SKeys(T), f, g);
  }

  lemma V_empty(T: TDB, nm: string)
    ensures V(T, {}, nm) == 0
  { SetSum_zero(SKeys(T), VTerm(T, {}, nm)); }

  lemma V_nonneg(T: TDB, K: set<int>, nm: string)
    ensures V(T, K, nm) >= 0
  {
    forall sk | sk in SKeys(T) ensures VTerm(T, K, nm)(sk) >= 0 { NW_nonneg(T, sk); }
    SetSum_nonneg(SKeys(T), VTerm(T, K, nm));
  }

  // Nations whose NWmap is empty contribute nothing.
  lemma V_drop_empty(T: TDB, K: set<int>, E: set<int>, nm: string)
    requires K !! E
    requires forall nk :: nk in E ==> NWmap(T, nk) == map[]
    ensures V(T, K + E, nm) == V(T, K, nm)
  {
    V_union(T, K, E, nm);
    forall sk | sk in SKeys(T) ensures VTerm(T, E, nm)(sk) == 0 {
      if SupNatOf(T)[sk] in E && NW(T, sk) != 0 {
        assert sk in NWmap(T, SupNatOf(T)[sk]);
      }
    }
    SetSum_zero(SKeys(T), VTerm(T, E, nm));
  }

  // ===================== Rows lemmas =====================

  lemma Pos_names(rows: seq<OutRow>, nm: string)
    requires forall r :: r in rows ==> r.numwait > 0
    ensures SumRows(rows, nm) >= 0
    ensures nm in NamesOf(rows) <==> SumRows(rows, nm) > 0
  {
    if |rows| > 0 {
      Pos_names(rows[1..], nm);
      assert rows == [rows[0]] + rows[1..];
      assert NamesOf(rows) == {rows[0].name} + NamesOf(rows[1..]);
    }
  }

  // Merged rows with per-name sums == V(T, K, .) answer Q21 for K.
  lemma Merge_answers(rows: seq<OutRow>, T: TDB, K: set<int>)
    requires forall r :: r in rows ==> r.numwait > 0
    requires forall nm :: SumRows(rows, nm) == V(T, K, nm)
    ensures RowsOk(MergeByNameAndSort(rows), T, K)
  {
    MergeByName_sorted(rows);
    forall x ensures x in MergeByNameAndSort(rows) <==> x in AnsSet(T, K) {
      Pos_names(rows, x.name);
      if x in AnsSet(T, K) {
        var nm :| nm in SupNames(T) && V(T, K, nm) > 0 && x == OutRow(nm, V(T, K, nm));
      }
      if x.name in NamesOf(rows) && x.numwait == SumRows(rows, x.name) {
        V_pos_name(T, K, x.name);
      }
    }
  }

  lemma NationRows_ok(st: TState, T: TDB, nk: int, en: seq<int>)
    requires NwOf(st, nk) == NWmap(T, nk)
    requires st.supplier_names == SupNameOf(T)
    requires nk in st.nation_numwait ==> IsEnum(en, st.nation_numwait[nk].Keys)
    ensures RowsOk(NationRows(st, nk, en), T, {nk})
  {
    if nk !in st.nation_numwait {
      forall nm ensures V(T, {nk}, nm) == 0 {
        V_single_nation(T, nk, nm);
        SetSum_empty(RowTerm(NWmap(T, nk), SupNameOf(T), nm));
      }
      assert forall x :: x !in AnsSet(T, {nk});
      assert StrictSorted([]);
    } else {
      var m := st.nation_numwait[nk];
      var rows := NationRowsLoop(m, st.supplier_names, en);
      NationRowsLoop_pos(m, st.supplier_names, en);
      forall nm ensures SumRows(rows, nm) == V(T, {nk}, nm) {
        NationRows_sum(m, st.supplier_names, en, nm);
        V_single_nation(T, nk, nm);
      }
      Merge_answers(rows, T, {nk});
    }
  }

  // ===================== EmitResult over loaded nations =====================

  lemma EmitRows_sum(nw: map<int, map<int, int>>, T: TDB, nks: seq<int>, ei: set<int> -> seq<int>, nm: string)
    requires forall s :: IsEnum(ei(s), s)
    requires NoDupSeq(nks)
    requires forall nk :: nk in nks && nk in nw ==> nw[nk] == NWmap(T, nk)
    requires forall nk :: nk in nks ==> nk in nw
    ensures SumRows(EmitRows(nw, SupNameOf(T), nks, ei), nm) == V(T, (set x | x in nks), nm)
    ensures forall r :: r in EmitRows(nw, SupNameOf(T), nks, ei) ==> r.numwait > 0
    decreases |nks|
  {
    if |nks| == 0 {
      V_empty(T, nm);
      assert (set x | x in nks) == {};
    } else {
      var nk := nks[0];
      assert NoDupSeq(nks[1..]) by {
        forall i, j | 0 <= i < j < |nks[1..]| ensures nks[1..][i] != nks[1..][j] { assert nks[i + 1] != nks[j + 1]; }
      }
      EmitRows_sum(nw, T, nks[1..], ei, nm);
      var head := NationRowsLoop(nw[nk], SupNameOf(T), ei(nw[nk].Keys));
      NationRows_sum(nw[nk], SupNameOf(T), ei(nw[nk].Keys), nm);
      V_single_nation(T, nk, nm);
      NationRowsLoop_pos(nw[nk], SupNameOf(T), ei(nw[nk].Keys));
      SumRows_app(head, EmitRows(nw, SupNameOf(T), nks[1..], ei), nm);
      var rest := set x | x in nks[1..];
      assert nk !in rest by {
        if nk in rest { var k :| 0 <= k < |nks[1..]| && nks[1..][k] == nk; assert nks[0] != nks[k + 1]; }
      }
      assert (set x | x in nks) == {nk} + rest by {
        forall x ensures x in (set x | x in nks) <==> x in {nk} + rest {
          if x in nks && x != nk { var k :| 0 <= k < |nks| && nks[k] == x; assert k > 0 && x in nks[1..]; }
        }
      }
      V_union(T, {nk}, rest, nm);
    }
  }

  lemma EmitResult_ok(st: TState, T: TDB, K: set<int>, ei: set<int> -> seq<int>)
    requires forall s :: IsEnum(ei(s), s)
    requires st.supplier_names == SupNameOf(T)
    requires forall nk :: nk in st.nation_numwait ==> nk in K && st.nation_numwait[nk] == NWmap(T, nk)
    requires forall nk :: nk in K && nk !in st.nation_numwait ==> NWmap(T, nk) == map[]
    ensures RowsOk(EmitResult(st, ei), T, K)
  {
    var nks := ei(st.nation_numwait.Keys);
    assert NoDupSeq(nks);
    var rows := EmitRows(st.nation_numwait, st.supplier_names, nks, ei);
    var L := set x | x in nks;
    assert L == st.nation_numwait.Keys;
    forall nm ensures SumRows(rows, nm) == V(T, K, nm) {
      EmitRows_sum(st.nation_numwait, T, nks, ei, nm);
      assert K == L + (K - L);
      V_drop_empty(T, L, K - L, nm);
    }
    EmitRows_sum(st.nation_numwait, T, nks, ei, "");
    Merge_answers(rows, T, K);
  }

  // ===================== Execute correctness =====================

  lemma Rows_eq_SQL(rows: seq<OutRow>, T: TDB, nn: string)
    requires KeysOk(T)
    requires RowsOk(rows, T, NationsNamed(T, nn))
    ensures rows == SQLRows(T, nn)
  {
    SQLRows_sorted_answer(T, nn);
    StrictSorted_unique(rows, SQLRows(T, nn));
  }

  lemma Empty_answer(T: TDB, K: set<int>)
    requires forall nk :: nk in K ==> NWmap(T, nk) == map[]
    ensures RowsOk([], T, K)
  {
    forall nm ensures V(T, K, nm) == 0 {
      assert {} !! K;
      V_drop_empty(T, {}, K, nm);
      assert {} + K == K;
      V_empty(T, nm);
    }
    assert forall x :: x !in AnsSet(T, K);
  }

  // LoadNations never fails under StoreInv and only touches nation_numwait.
  lemma LoadNations_some(st: TState, snap: map<int, TImage>, nks: seq<int>, T: TDB)
    requires StoreInv(snap, T)
    requires forall nk :: nk in nks ==> NkRange(nk)
    ensures LoadNations(st, snap, nks).SomeState?
    ensures LoadNations(st, snap, nks).st == st.(nation_numwait := LoadNations(st, snap, nks).st.nation_numwait)
    decreases |nks|
  {
    if |nks| > 0 {
      var nk := nks[0];
      assert nk in nks && NkRange(nk);
      var pid := NationPartition(nk);
      assert IsNatPid(pid);
      if pid in snap {
        assert ImageOk(pid, snap[pid], T);
        var img := snap[pid];
        var st2 := if nk in img.state.nation_numwait
                   then st.(nation_numwait := st.nation_numwait[nk := img.state.nation_numwait[nk]])
                   else st;
        LoadNations_some(st2, snap, nks[1..], T);
      } else {
        LoadNations_some(st, snap, nks[1..], T);
      }
    }
  }

  lemma LoadNations_mono(st: TState, snap: map<int, TImage>, nks: seq<int>)
    requires LoadNations(st, snap, nks).SomeState?
    ensures st.nation_numwait.Keys <= LoadNations(st, snap, nks).st.nation_numwait.Keys
    decreases |nks|
  {
    if |nks| > 0 {
      var nk := nks[0];
      var pid := NationPartition(nk);
      if pid in snap {
        var img := snap[pid];
        var st2 := if nk in img.state.nation_numwait
                   then st.(nation_numwait := st.nation_numwait[nk := img.state.nation_numwait[nk]])
                   else st;
        LoadNations_mono(st2, snap, nks[1..]);
      } else {
        LoadNations_mono(st, snap, nks[1..]);
      }
    }
  }

  lemma LoadNations_nw(st: TState, snap: map<int, TImage>, nks: seq<int>, T: TDB)
    requires StoreInv(snap, T)
    requires forall nk :: nk in nks ==> NkRange(nk)
    requires forall nk :: nk in st.nation_numwait ==> st.nation_numwait[nk] == NWmap(T, nk)
    requires LoadNations(st, snap, nks).SomeState?
    ensures var nw := LoadNations(st, snap, nks).st.nation_numwait;
            (forall nk :: nk in nw ==> nw[nk] == NWmap(T, nk)) &&
            (forall nk :: nk in nw ==> nk in st.nation_numwait || nk in nks) &&
            (forall nk :: nk in nks && nk !in nw ==> NWmap(T, nk) == map[] || nk in st.nation_numwait)
    decreases |nks|
  {
    if |nks| > 0 {
      var nk := nks[0];
      assert nk in nks && NkRange(nk);
      var pid := NationPartition(nk);
      assert IsNatPid(pid);
      if pid in snap {
        assert ImageOk(pid, snap[pid], T);
        var img := snap[pid];
        assert NwOf(img.state, nk) == NWmap(T, nk);
        var st2 := if nk in img.state.nation_numwait
                   then st.(nation_numwait := st.nation_numwait[nk := img.state.nation_numwait[nk]])
                   else st;
        LoadNations_nw(st2, snap, nks[1..], T);
        LoadNations_mono(st2, snap, nks[1..]);
      } else {
        assert NWmap(T, nk) == map[];
        LoadNations_nw(st, snap, nks[1..], T);
      }
    }
  }

  lemma Fast_ok(T: TDB, nn: string, snap: map<int, TImage>)
    requires KeysOk(T) && StoreInv(snap, T)
    requires var fpid := NationNamePartition(nn);
             fpid in snap && snap[fpid].kind == NationK && snap[fpid].nation_name == nn
    ensures snap[NationNamePartition(nn)].output_rows == SQLRows(T, nn)
  {
    var fpid := NationNamePartition(nn);
    assert IsNamePid(fpid);
    assert ImageOk(fpid, snap[fpid], T);
    Rows_eq_SQL(snap[fpid].output_rows, T, nn);
  }

  lemma Idx_facts(T: TDB, nn: string, snap: map<int, TImage>)
    requires DomOk(T) && StoreInv(snap, T)
    requires kNationNameIndex in snap
    ensures var idx := snap[kNationNameIndex].state.nation_name_to_keys;
            (nn !in idx || |idx[nn]| == 0 ==> NationsNamed(T, nn) == {}) &&
            (nn in idx ==> (NoDupSeq(idx[nn]) &&
                            (forall x :: x in idx[nn] <==> x in NationsNamed(T, nn)) &&
                            (forall x :: x in idx[nn] ==> NkRange(x))))
  {
    assert ImageOk(kNationNameIndex, snap[kNationNameIndex], T);
    var idx := snap[kNationNameIndex].state.nation_name_to_keys;
    if nn in idx {
      forall x | x in idx[nn] ensures NkRange(x) {
        assert x in NationsNamed(T, nn);
        var n :| n in T.nats && n.name == nn && n.nk == x;
      }
      if |idx[nn]| == 0 {
        forall x | x in NationsNamed(T, nn) ensures false { assert x in idx[nn]; }
      }
    }
  }

  lemma SlowMulti_ok(T: TDB, nn: string, snap: map<int, TImage>, ei: set<int> -> seq<int>)
    requires KeysOk(T) && DomOk(T) && StoreInv(snap, T)
    requires forall s :: IsEnum(ei(s), s)
    requires kNationNameIndex in snap
    requires var idx := snap[kNationNameIndex].state.nation_name_to_keys;
             nn in idx && LoadStateForNations(snap, idx[nn]).SomeState?
    ensures var idx := snap[kNationNameIndex].state.nation_name_to_keys;
            EmitResult(LoadStateForNations(snap, idx[nn]).st, ei) == SQLRows(T, nn)
  {
    var idx := snap[kNationNameIndex].state.nation_name_to_keys;
    var nks := idx[nn];
    var K := NationsNamed(T, nn);
    Idx_facts(T, nn, snap);
    assert ImageOk(kSupplierMetadata, snap[kSupplierMetadata], T);
    var names := snap[kSupplierMetadata].state.supplier_names;
    var st0 := EmptyState().(supplier_names := names);
    LoadNations_some(st0, snap, nks, T);
    LoadNations_nw(st0, snap, nks, T);
    var st := LoadStateForNations(snap, nks).st;
    forall nk | nk in K && nk !in st.nation_numwait ensures NWmap(T, nk) == map[] {
      assert nk in nks;
    }
    EmitResult_ok(st, T, K, ei);
    Rows_eq_SQL(EmitResult(st, ei), T, nn);
  }

  lemma SlowSingle_ok(T: TDB, nn: string, snap: map<int, TImage>)
    requires KeysOk(T) && DomOk(T) && StoreInv(snap, T)
    requires kNationNameIndex in snap
    requires var idx := snap[kNationNameIndex].state.nation_name_to_keys;
             nn in idx && |idx[nn]| == 1
    ensures var nk := snap[kNationNameIndex].state.nation_name_to_keys[nn][0];
            var pid := NationPartition(nk);
            (if pid in snap && snap[pid].kind == NationK && snap[pid].nationkey == nk
             then snap[pid].output_rows else []) == SQLRows(T, nn)
  {
    var idx := snap[kNationNameIndex].state.nation_name_to_keys;
    Idx_facts(T, nn, snap);
    var nk := idx[nn][0];
    var K := NationsNamed(T, nn);
    assert K == {nk} by {
      forall x ensures x in K <==> x == nk {
        if x in K { assert x in idx[nn]; assert idx[nn] == [nk]; }
        if x == nk { assert idx[nn][0] in idx[nn]; }
      }
    }
    assert nk in idx[nn];
    assert NkRange(nk);
    var pid := NationPartition(nk);
    assert IsNatPid(pid);
    if pid in snap {
      assert ImageOk(pid, snap[pid], T);
      Rows_eq_SQL(snap[pid].output_rows, T, nn);
    } else {
      Empty_answer(T, K);
      Rows_eq_SQL([], T, nn);
    }
  }

  lemma SlowEmpty_ok(T: TDB, nn: string, snap: map<int, TImage>)
    requires KeysOk(T) && DomOk(T) && StoreInv(snap, T)
    requires kNationNameIndex in snap
    requires var idx := snap[kNationNameIndex].state.nation_name_to_keys;
             nn !in idx || |idx[nn]| == 0
    ensures SQLRows(T, nn) == []
  {
    Idx_facts(T, nn, snap);
    Empty_answer(T, NationsNamed(T, nn));
    Rows_eq_SQL([], T, nn);
  }

  // ExecCorrect (hit path): an emitted result is the RelNode Eval_Spec result.
  lemma Execute_correct(T: TDB, nn: string, snap: map<int, TImage>, ei: set<int> -> seq<int>)
    requires KeysOk(T) && DomOk(T)
    requires StoreInv(snap, T)
    requires forall s :: IsEnum(ei(s), s)
    ensures Execute(ArgText(nn), false, true, snap, ei).Emitted? ==>
            Execute(ArgText(nn), false, true, snap, ei).rows == SQLRows(T, nn)
  {
    var fpid := NationNamePartition(nn);
    if fpid in snap && snap[fpid].kind == NationK && snap[fpid].nation_name == nn &&
       |snap[fpid].output_rows| > 0 {
      Fast_ok(T, nn, snap);
    } else if kNationNameIndex in snap && snap[kNationNameIndex].kind == NationNameIndex {
      var idx := snap[kNationNameIndex].state.nation_name_to_keys;
      if nn !in idx || |idx[nn]| == 0 {
        SlowEmpty_ok(T, nn, snap);
      } else if |idx[nn]| != 1 {
        if LoadStateForNations(snap, idx[nn]).SomeState? { SlowMulti_ok(T, nn, snap, ei); }
      } else {
        SlowSingle_ok(T, nn, snap);
      }
    }
  }
}
