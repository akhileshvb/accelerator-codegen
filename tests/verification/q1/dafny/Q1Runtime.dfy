// Stage 5 (model): the Q1 Postgres runtime with the cross-backend sync
// protocol, hit => SQL result.  Same recipe as q21/dafny/Q21Runtime.dfy: the
// protocol soundness is the query-generic substrate/dafny/AuxSyncProtocol.dfy;
// this file supplies Q1's store invariant, Build / own-commit steps and the
// hit / fallback results against the RelNode Eval_Spec (Q1ExecExact.Q1Rows).
//
//   Q1Store       Valid(days) | Invalid: one backend's process-local Q1 store
//                 (the twin DayStore of module.cpp).
//   SInvQ1        marker invariant + Valid(days) represents dbof(R):
//                 TwinStoreOk, OrderExact, Flatten(days) == FoldTwin(dbof(R)).
//   Build_step_Q1   Rebuild (BuildStateForDays over the build snapshot's rows)
//                   establishes SInvQ1 for R = writers visible to it.
//   Commit_step_Q1  own commit applying the captured signed deltas keeps
//                   SInvQ1 for R + {A} (or the store is invalidated).
//   Hit_correct_Q1 / Fallback_correct_Q1: Execute == Q1Rows of the database the
//                   statement sees (exact, including avg(numeric)).
//
// TCB used here (see q1/RESIDUALS.txt): pg_mvcc / pg_callbacks / pg_atomics
// (via AuxSyncProtocol), runtime_scan (the SPI scan under a snapshot returns
// exactly that snapshot's lineitem rows), runtime_capture (CaptureOkQ1).

include "Q1ExecExact.dfy"
include "Q1TwinAuxClose.dfy"
include "../../substrate/dafny/AuxSyncProtocol.dfy"

module Q1Runtime {
  import opened AuxSyncProtocol
  import A = Q1Aux
  import Twin = Q1ModuleTwin
  import Ref = Q1TwinAuxRefinement
  import C = Q1TwinAuxClose
  import B = Q1ResultSpecBridge
  import X = Q1ExecExact
  import Spec = Q1ResultSpec

  // ===================== Aux fold depends only on the multiset =====================

  lemma RemoveAt_multiset(t: seq<A.Row>, i: nat)
    requires i < |t|
    ensures multiset(A.RemoveAt(t, i)) == multiset(t) - multiset{t[i]}
  {
    assert t == t[..i] + [t[i]] + t[i + 1..];
    assert multiset(t) == multiset(t[..i]) + multiset{t[i]} + multiset(t[i + 1..]);
  }

  lemma LastSplit(t: seq<A.Row>)
    requires |t| > 0
    ensures t == t[..|t| - 1] + [t[|t| - 1]]
    ensures multiset(t) == multiset(t[..|t| - 1]) + multiset{t[|t| - 1]}
  {
    assert t == t[..|t| - 1] + [t[|t| - 1]];
  }

  lemma ApplyRow_cancel(s: A.Stats, r: A.Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures A.ApplyRow(A.ApplyRow(s, r, -1), r, 1) == s
  {}

  // Contrib(t2, k) in terms of its prefix (definition unfolded once).
  lemma ContribLast(t: seq<A.Row>, k: A.CellKey)
    requires A.RowsOk(t) && |t| > 0
    ensures var r := t[|t| - 1];
            A.Contrib(t, k) ==
              (if A.CellKey(r.shipdate, A.RowKey(r)) == k
               then A.ApplyRow(A.Contrib(t[..|t| - 1], k), r, 1)
               else A.Contrib(t[..|t| - 1], k))
  {}

  lemma {:isolate_assertions} Contrib_perm(t1: seq<A.Row>, t2: seq<A.Row>, k: A.CellKey)
    requires A.RowsOk(t1) && A.RowsOk(t2) && multiset(t1) == multiset(t2)
    ensures A.Contrib(t1, k) == A.Contrib(t2, k)
    decreases |t2|
  {
    if |t2| == 0 {
      assert |t1| == 0 by { assert |multiset(t1)| == |t1|; }
    } else {
      var n := |t2|;
      var r := t2[n - 1];
      var pre := t2[..n - 1];
      LastSplit(t2);
      assert r in multiset(t1);
      var i :| 0 <= i < |t1| && t1[i] == r;
      var t1r := A.RemoveAt(t1, i);
      RemoveAt_multiset(t1, i);
      assert multiset(t1r) == multiset(pre);
      assert A.RowsOk(t1r);
      assert A.RowsOk(pre);
      Contrib_perm(t1r, pre, k);
      A.ContribRemoveAt(t1, i, k);
      ContribLast(t2, k);
      if A.CellKey(r.shipdate, A.RowKey(r)) == k {
        ApplyRow_cancel(A.Contrib(t1, k), r);
      }
    }
  }

  lemma Inv_perm(store: A.Store, t1: seq<A.Row>, t2: seq<A.Row>)
    requires A.RowsOk(t1) && A.RowsOk(t2) && multiset(t1) == multiset(t2)
    requires A.Inv(store, t1)
    ensures A.Inv(store, t2)
  {
    forall k: A.CellKey ensures A.Contrib(t1, k) == A.Contrib(t2, k) {
      Contrib_perm(t1, t2, k);
    }
    A.FoldExtensional(t1, t2);
  }

  // ===================== Captured signed deltas =====================

  datatype Op = Op(r: A.Row, sign: int)

  ghost predicate OpsRowsOk(ops: seq<Op>)
  {
    forall i :: 0 <= i < |ops| ==>
      0 <= ops[i].r.rf < 256 && 0 <= ops[i].r.ls < 256 && (ops[i].sign == 1 || ops[i].sign == -1)
  }

  ghost function Deltas(ops: seq<Op>): seq<Twin.LineitemDelta>
    requires OpsRowsOk(ops)
    decreases |ops|
  {
    if |ops| == 0 then [] else [Ref.RowToDelta(ops[0].r, ops[0].sign)] + Deltas(ops[1..])
  }

  // Multiset of rows after the ops; deletes must hit a present row.
  ghost predicate OpsOk(ms: multiset<A.Row>, ops: seq<Op>)
    decreases |ops|
  {
    OpsRowsOk(ops) &&
    (|ops| > 0 ==>
      (ops[0].sign == -1 ==> ops[0].r in ms) &&
      OpsOk(if ops[0].sign == 1 then ms + multiset{ops[0].r} else ms - multiset{ops[0].r}, ops[1..]))
  }

  ghost function MsAfter(ms: multiset<A.Row>, ops: seq<Op>): multiset<A.Row>
    decreases |ops|
  {
    if |ops| == 0 then ms
    else MsAfter(if ops[0].sign == 1 then ms + multiset{ops[0].r} else ms - multiset{ops[0].r}, ops[1..])
  }

  lemma ApplyTwinDelta_eq(store: A.Store, r: A.Row, sign: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures Ref.ApplyTwinDelta(store, Ref.RowToDelta(r, sign)) == A.ApplyDelta(store, r, sign)
  {}

  lemma Apply_ops_Inv(store: A.Store, t: seq<A.Row>, ops: seq<Op>)
    requires A.RowsOk(t) && A.Inv(store, t) && OpsOk(multiset(t), ops)
    ensures exists t' :: A.RowsOk(t') && multiset(t') == MsAfter(multiset(t), ops) &&
                         A.Inv(Ref.ApplyTwinDeltaSeq(store, Deltas(ops)), t')
    decreases |ops|
  {
    if |ops| == 0 {
      assert A.Inv(Ref.ApplyTwinDeltaSeq(store, Deltas(ops)), t);
    } else {
      var op := ops[0];
      ApplyTwinDelta_eq(store, op.r, op.sign);
      var store1 := A.ApplyDelta(store, op.r, op.sign);
      assert Ref.ApplyTwinDeltaSeq(store, Deltas(ops)) == Ref.ApplyTwinDeltaSeq(store1, Deltas(ops[1..]));
      if op.sign == 1 {
        A.InvPreservedByInsert(store, t, op.r);
        var t1 := t + [op.r];
        assert multiset(t1) == multiset(t) + multiset{op.r};
        Apply_ops_Inv(store1, t1, ops[1..]);
      } else {
        assert op.r in multiset(t);
        var i :| 0 <= i < |t| && t[i] == op.r;
        A.InvPreservedByDeleteAt(store, t, i);
        var t1 := A.RemoveAt(t, i);
        RemoveAt_multiset(t, i);
        Apply_ops_Inv(store1, t1, ops[1..]);
      }
    }
  }

  lemma Apply_ops_Fold(t: seq<A.Row>, ops: seq<Op>, t2: seq<A.Row>)
    requires A.RowsOk(t) && A.RowsOk(t2) && OpsOk(multiset(t), ops)
    requires multiset(t2) == MsAfter(multiset(t), ops)
    ensures Ref.ApplyTwinDeltaSeq(A.FoldTwin(t), Deltas(ops)) == A.FoldTwin(t2)
  {
    Apply_ops_Inv(A.FoldTwin(t), t, ops);
    var t' :| A.RowsOk(t') && multiset(t') == MsAfter(multiset(t), ops) &&
              A.Inv(Ref.ApplyTwinDeltaSeq(A.FoldTwin(t), Deltas(ops)), t');
    Inv_perm(Ref.ApplyTwinDeltaSeq(A.FoldTwin(t), Deltas(ops)), t', t2);
  }

  // ===================== Store invariant =====================

  datatype Q1Store = Valid(days: Twin.DayStore) | Invalid

  ghost predicate DbOkQ1(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> seq<A.Row>)
  {
    DbRel(Ws, D, dbof) && (forall V :: A.RowsOk(dbof(V)))
  }

  ghost predicate SInvQ1(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> seq<A.Row>,
                         M: Marker, R: set<Writer>, st: Q1Store)
  {
    MInv(Ws, D, M, R) && A.RowsOk(dbof(R)) &&
    (st.Valid? ==>
      C.TwinStoreOk(st.days) && C.OrderExact(st.days) &&
      Ref.TwinAuxRel(st.days, A.FoldTwin(dbof(R))))
  }

  ghost predicate InputsOk(inputs: seq<Twin.LineitemInput>)
  {
    forall i :: 0 <= i < |inputs| ==>
      0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
      inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
      inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
  }

  // Runtime::Rebuild: counter read t0 < fresh snapshot tsb < counter reads;
  // the SPI scan under tsb returns exactly its lineitem rows (TCB runtime_scan).
  lemma Build_step_Q1(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> seq<A.Row>,
                      Sb: PgSnap, t0: int, tsb: int, ti: map<Slot, int>, tq: map<Slot, int>,
                      c: map<Slot, nat>, inputs: seq<Twin.LineitemInput>)
    requires History(Ws) && DbOkQ1(Ws, D, dbof)
    requires SnapAt(Ws, Sb, tsb) && t0 < tsb && Reads(D, tsb, ti, tq) && D <= c.Keys
    requires forall k :: k in D ==> |Counted(Ws, k, t0)| == c[k]
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) == {} && |Counted(Ws, k, tq[k])| == c[k]
    requires InputsOk(inputs) && Ref.InputsToRows(inputs) == dbof(VisibleAt(Ws, tsb))
    ensures SInvQ1(Ws, D, dbof, BuildMarker(Sb, c, D, t0), VisibleAt(Ws, tsb) * Rel(Ws, D),
                   Valid(Twin.BuildStateForDays({}, [], false, [], false, inputs)))
  {
    Build_sound(Ws, D, Sb, t0, tsb, ti, tq, c);
    Build_db(Ws, D, dbof, tsb);
    Ref.BuildStateForDays_lineitems_RebuildOracleCorrect(inputs);
    C.BuildStateForDays_preserves_TwinStoreOk({}, [], false, [], false, inputs);
  }

  // TCB runtime_capture: A's captured signed row deltas turn the database it
  // was based on (all writers visible at PRE_COMMIT) into that database + A.
  ghost predicate CaptureOkQ1(db: seq<A.Row>, db2: seq<A.Row>, ops: seq<Op>)
  {
    OpsOk(multiset(db), ops) && multiset(db2) == MsAfter(multiset(db), ops)
  }

  lemma Commit_step_Q1(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> seq<A.Row>,
                       M: Marker, R: set<Writer>, days: Twin.DayStore, A_: Writer, tp: int,
                       ti: map<Slot, int>, tq: map<Slot, int>, ops: seq<Op>)
    requires History(Ws) && DbOkQ1(Ws, D, dbof) && SInvQ1(Ws, D, dbof, M, R, Valid(days))
    requires A_ in Ws && A_.committed && A_.slots * D != {}
    requires forall k :: k in D ==> M.tau[k] <= tp
    requires forall k :: k in A_.slots ==> A_.a[k] < tp
    requires Reads(D, tp, ti, tq) && forall k :: k in D ==> tq[k] < A_.v
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) <= {A_} && |Counted(Ws, k, tq[k])| == M.cnt[k]
    requires forall k :: k in D * A_.slots ==> |Counted(Ws, k, A_.s[k] - 1)| == M.cnt[k]
    requires CaptureOkQ1(dbof(VisibleAt(Ws, tp)), dbof(VisibleAt(Ws, tp) + {A_}), ops)
    ensures SInvQ1(Ws, D, dbof, Marker(M.base, M.own + {A_.xid}, OwnSeq(M, D, A_), OwnTau(M, D, A_)),
                   R + {A_}, Valid(Twin.ApplyDeltaSeq(days, Deltas(ops))))
    ensures SInvQ1(Ws, D, dbof, Marker(M.base, M.own + {A_.xid}, OwnSeq(M, D, A_), OwnTau(M, D, A_)),
                   R + {A_}, Invalid)
  {
    OwnCommit_db(Ws, D, dbof, M, R, A_, tp, ti, tq);
    C.Q1Twin_Maint_ApplyDeltaSeq_closed(days, A.FoldTwin(dbof(R)), Deltas(ops));
    Apply_ops_Fold(dbof(R), ops, dbof(R + {A_}));
  }

  // ===================== Execute =====================

  lemma Cutoff_of(c: int)
    ensures Spec.Cutoff(Spec.BaseDate - c) == c
  {}

  // A hit for the statement snapshot S (taken at ts; counters read after ts)
  // returns Q1Rows of exactly the database the statement sees (Tv; its
  // transaction has no captured dependency writes, TCB pg_mvcc).
  lemma Hit_correct_Q1(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> seq<A.Row>,
                       M: Marker, R: set<Writer>, days: Twin.DayStore,
                       S: PgSnap, ts: int, ti: map<Slot, int>, tq: map<Slot, int>,
                       args: Twin.QueryArgs, base_days: int, built: Twin.DayStore, Tv: seq<A.Row>)
    requires History(Ws) && DbOkQ1(Ws, D, dbof) && SInvQ1(Ws, D, dbof, M, R, Valid(days))
    requires SnapAt(Ws, S, ts) && Reads(D, ts, ti, tq) && (forall k :: k in D ==> M.tau[k] <= tq[k])
    requires HitCheck(Ws, D, M, S, ti, tq)
    requires Tv == dbof(VisibleAt(Ws, ts))
    requires args.has_offset
    ensures A.RowsOk(Tv)
    ensures Twin.Execute(args, base_days, true, true, days, built)
         == X.Q1Rows(Twin.TpchQ1CutoffDays(base_days, args.day_offset), B.AuxRowsToSpec(Tv))
  {
    Hit_db(Ws, D, dbof, M, R, S, ts, ti, tq);
    assert Tv == dbof(R);
    var store := A.FoldTwin(Tv);
    assert Ref.TwinExecRefinesAux_Premises(days, store, Tv);
    C.Q1Twin_Execute_hit_closed(args, base_days, days, store, Tv, built);
    var c := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
    Cutoff_of(c);
    X.Q1ExecExact(store, Tv, Spec.BaseDate - c);
  }

  // Invalid store or failed load: Execute's fallback builds from the
  // statement's own scan (TCB runtime_scan) and is exact as well.
  lemma Fallback_correct_Q1(args: Twin.QueryArgs, base_days: int, store_days: Twin.DayStore,
                            inputs: seq<Twin.LineitemInput>, valid: bool, load_ok: bool)
    requires args.has_offset && (!valid || !load_ok) && InputsOk(inputs)
    ensures A.RowsOk(Ref.InputsToRows(inputs))
    ensures Twin.Execute(args, base_days, valid, load_ok, store_days,
                         Twin.BuildStateForDays({}, [], false, [], false, inputs))
         == X.Q1Rows(Twin.TpchQ1CutoffDays(base_days, args.day_offset),
                     B.AuxRowsToSpec(Ref.InputsToRows(inputs)))
  {
    C.Q1Twin_Execute_fallback_from_BuildState_lineitems_closed(args, base_days, store_days, inputs, valid, load_ok);
    var rows := Ref.InputsToRows(inputs);
    var c := Twin.TpchQ1CutoffDays(base_days, args.day_offset);
    Cutoff_of(c);
    X.Q1ExecExact(A.FoldTwin(rows), rows, Spec.BaseDate - c);
  }
}
