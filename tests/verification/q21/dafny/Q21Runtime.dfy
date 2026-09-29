// Stage 5 (model): the Q21 Postgres runtime with the cross-backend sync
// protocol, hit => SQL result.  No environment premises about other backends,
// savepoints or build timing: those are now enforced by the runtime
// (src/stateful/postgres/aux_sync.cpp, src/generated/postgres/runtime.cpp,
// postgres_xact_callbacks.cpp) and modelled below.
//
// ---------------------------------------------------------------- Stores
// PStore is ONE backend's process-local PostgresStateStore:
//   PValid(m)  every indexed Q21 slot Valid, m = the published images;
//   PInvalid   some slot invalidated (NeedsRebuild -> no hit, rebuild).
//
// ---------------------------------------------------------------- History
// The whole execution is a finite set Ws of writer transactions (all
// backends).  Each writer W has, per table slot k it wrote (W.slots):
//   a[k]  inflight[k]++          (capture trigger, before W commits)
//   v     commit becomes visible  (ProcArrayEndTransaction; committed only)
//   s[k]  seq[k]++               (XACT_EVENT_COMMIT, after v)
//   d[k]  inflight[k]--          (after s[k]; at ABORT for aborted writers)
// History(Ws) states exactly these orderings (WProto) plus: xids distinct, and
// per slot the seq increments of different writers are distinct atomic RMWs.
// Counter reads at time t observe Inflight(Ws,k,t) / Counted(Ws,k,t).
//
// Snapshots are modelled by content (xmax, running = xip ∪ subxip) as the
// runtime reads them; SnapAt(Ws,S,ts) is PostgreSQL's MVCC contract for a
// non-overflowed MVCC snapshot taken at time ts (TCB pg_mvcc).
//
// ---------------------------------------------------------------- Theorems
//   Build_sound      Rebuild (counter read t0 < fresh snapshot ts_b < build
//                    < counter read t1) establishes the sync marker for
//                    R = writers visible at ts_b.
//   OwnCommit_sound  the backend's own commit of A keeps the marker for
//                    R ∪ {A} when the PRE_COMMIT and COMMIT checks pass, and
//                    the maintenance base is exactly R.
//   Hit_sound        a passing hit check for statement snapshot S (taken at
//                    ts, counters read after ts) implies the dependency
//                    writers visible to S are exactly R.
//   Runtime_hit_correct / Runtime_fallback_correct: the rows returned are
//                    SQLRows(Tv) = Eval_Spec(LogicalSpec_Q21_rel, Tv).
//   Build_step / Commit_step: the store invariant StoreInv(store, dbof(R)).

include "Q21Maint.dfy"
include "Q21Perm.dfy"
include "Q21SqlAgg.dfy"
include "../../substrate/dafny/AuxSyncProtocol.dfy"

module Q21Runtime {
  import opened Q21Types
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened Q21Answer
  import opened Q21Inv
  import opened Q21Build
  import opened Q21Delta
  import opened Q21Maint
  import opened Q21Perm
  import opened RelEvalLogicalSpec
  import Agg = Q21SqlAgg
  import Q21RelNodeData
  import opened AuxSyncProtocol

  // ===================== Store =====================

  datatype PStore = PValid(m: map<int, TImage>) | PInvalid

  function Snap(s: PStore): map<int, TImage> { if s.PValid? then s.m else map[] }

  ghost predicate RuntimeInv(s: PStore, Tc: TDB)
  { s.PValid? ==> StoreInv(s.m, Tc) }

  function InstallBuilt(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): PStore
    requires EnumOracle(ei, es)
  { PValid(InstallMap(BuildImages(T, ei, es))) }

  function CommitTxn(s: PStore, b: TxnBuffer, dec1: Action, dec2: Action,
                     ei: set<int> -> seq<int>, es: set<string> -> seq<string>): PStore
    requires EnumOracle(ei, es)
  {
    if !b.pending then s
    else
      var out := StageMaintenance(b, dec1, dec2, Snap(s), Snap(s), ei, es);
      if out.invalidate_all then PInvalid
      else if out.action == ApplyDelta then
        (if s.PValid? then PValid(PublishMap(s.m, out.updates)) else PInvalid)
      else s
  }

  lemma Install_inv(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es) && KeysOk(T) && DomOk(T)
    ensures RuntimeInv(InstallBuilt(T, ei, es), T)
  { Build_establishes_Inv(T, ei, es); }

  function Committed(Tc: TDB, b: TxnBuffer): TDB
  { TDB(Tc.sup, ApplyLi(Tc.li, b.lineitem_deltas), ApplyOrd(Tc.ords, b.order_deltas), Tc.nats) }

  // The captured transaction buffer b takes the committed DB Tc to Tc2: either
  // the buffer demands a rebuild, or its deltas are valid on Tc and applying
  // them yields Tc2 up to row order (TCB runtime_capture + pg_mvcc).
  ghost predicate CaptureOk(Tc: TDB, Tc2: TDB, b: TxnBuffer)
  {
    (b.pending && b.requires_rebuild) ||
    (TxnOk(Tc, Committed(Tc, b), b) && TPerm(Committed(Tc, b), Tc2) && KeysOk(Tc2) &&
     (!b.pending ==> |b.lineitem_deltas| == 0 && |b.order_deltas| == 0))
  }

  lemma Commit_inv(s: PStore, Tc: TDB, Tc2: TDB, b: TxnBuffer, dec1: Action, dec2: Action,
                   ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires RuntimeInv(s, Tc)
    requires CaptureOk(Tc, Tc2, b)
    ensures RuntimeInv(CommitTxn(s, b, dec1, dec2, ei, es), Tc2)
  {
    if b.pending && b.requires_rebuild {
      assert StageMaintenance(b, dec1, dec2, Snap(s), Snap(s), ei, es).invalidate_all;
      return;
    }
    var Tm := Committed(Tc, b);
    var r := CommitTxn(s, b, dec1, dec2, ei, es);
    if r.PValid? {
      if !b.pending || (var out := StageMaintenance(b, dec1, dec2, Snap(s), Snap(s), ei, es);
                        !out.invalidate_all && out.action != ApplyDelta) {
        if b.pending { Maint_other_arms(b, dec1, dec2, Snap(s), ei, es); }
        assert Tm == Tc;
        StoreInv_perm(s.m, Tc, Tc2);
      } else {
        Maint_ApplyDelta(Tc, Tm, b, dec1, dec2, s.m, ei, es);
        StoreInv_perm(r.m, Tm, Tc2);
      }
    }
  }

  // ===================== Store invariant across steps =====================

  // dbof(V): the dependency-table contents of the database whose committed
  // writers are V (TCB pg_mvcc + runtime_capture: only writers that
  // registered a dependency slot change dependency tables).
  ghost predicate DbOf(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> TDB)
  {
    (forall V :: V <= Ws ==> dbof(V) == dbof(V * Rel(Ws, D))) &&
    (forall ts: int :: KeysOk(dbof(VisibleAt(Ws, ts))) && DomOk(dbof(VisibleAt(Ws, ts))))
  }

  ghost predicate SInv(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> TDB,
                       M: Marker, R: set<Writer>, s: PStore)
  { AuxSyncProtocol.MInv(Ws, D, M, R) && RuntimeInv(s, dbof(R)) }

  lemma Build_step(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> TDB, Sb: PgSnap,
                   t0: int, tsb: int, ti: map<Slot, int>, tq: map<Slot, int>, c: map<Slot, nat>,
                   ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es) && History(Ws) && DbOf(Ws, D, dbof)
    requires SnapAt(Ws, Sb, tsb) && t0 < tsb && Reads(D, tsb, ti, tq) && D <= c.Keys
    requires forall k :: k in D ==> |Counted(Ws, k, t0)| == c[k]
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) == {} && |Counted(Ws, k, tq[k])| == c[k]
    ensures var R := VisibleAt(Ws, tsb) * Rel(Ws, D);
            SInv(Ws, D, dbof, BuildMarker(Sb, c, D, t0), R, InstallBuilt(dbof(VisibleAt(Ws, tsb)), ei, es))
  {
    Build_sound(Ws, D, Sb, t0, tsb, ti, tq, c);
    var V := VisibleAt(Ws, tsb);
    assert dbof(V) == dbof(V * Rel(Ws, D));
    Install_inv(dbof(V), ei, es);
  }

  lemma Commit_step(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> TDB,
                    M: Marker, R: set<Writer>, s: PStore, A: Writer, tp: int,
                    ti: map<Slot, int>, tq: map<Slot, int>, b: TxnBuffer, dec1: Action, dec2: Action,
                    ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es) && History(Ws) && DbOf(Ws, D, dbof) && SInv(Ws, D, dbof, M, R, s)
    requires A in Ws && A.committed && A.slots * D != {}
    requires forall k :: k in D ==> M.tau[k] <= tp
    requires forall k :: k in A.slots ==> A.a[k] < tp
    requires Reads(D, tp, ti, tq) && forall k :: k in D ==> tq[k] < A.v
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) <= {A} && |Counted(Ws, k, tq[k])| == M.cnt[k]
    requires forall k :: k in D * A.slots ==> |Counted(Ws, k, A.s[k] - 1)| == M.cnt[k]
    // TCB: A's captured buffer turns the DB it was based on (every writer
    // visible at PRE_COMMIT) into that DB plus A.
    requires CaptureOk(dbof(VisibleAt(Ws, tp)), dbof(VisibleAt(Ws, tp) + {A}), b)
    ensures SInv(Ws, D, dbof, Marker(M.base, M.own + {A.xid}, OwnSeq(M, D, A), OwnTau(M, D, A)),
                 R + {A}, CommitTxn(s, b, dec1, dec2, ei, es))
  {
    OwnCommit_sound(Ws, D, M, R, A, tp, ti, tq);
    var V := VisibleAt(Ws, tp);
    assert V * Rel(Ws, D) == R;
    assert dbof(V) == dbof(R);
    assert A in Rel(Ws, D);
    assert (V + {A}) * Rel(Ws, D) == R + {A};
    assert dbof(V + {A}) == dbof(R + {A});
    Commit_inv(s, dbof(R), dbof(R + {A}), b, dec1, dec2, ei, es);
  }

  // ===================== Execute =====================

  // Runtime::Execute: hit => TpchQ21Module::Execute (clean transaction, store
  // valid), else ExecuteFallback on the statement snapshot's database Tv.
  function RuntimeExecute(hit: bool, s: PStore, nn: string, Tv: TDB,
                          ei: set<int> -> seq<int>, es: set<string> -> seq<string>): seq<OutRow>
    requires EnumOracle(ei, es)
  {
    if hit then
      var r := Execute(ArgText(nn), false, s.PValid?, Snap(s), ei);
      if r.Emitted? then r.rows else ExecuteFallback(Tv, ArgText(nn), ei, es)
    else ExecuteFallback(Tv, ArgText(nn), ei, es)
  }

  lemma Runtime_fallback_correct(s: PStore, nn: string, Tv: TDB,
                                 ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es) && KeysOk(Tv) && DomOk(Tv)
    ensures RuntimeExecute(false, s, nn, Tv, ei, es) == SQLRows(Tv, nn)
  { ExecuteFallback_correct(Tv, nn, ei, es); }

  // Q21ExecCorrectOrdered (runtime): a hit for the statement snapshot S
  // (taken at ts; counters read after ts) returns SQLRows of the database the
  // statement sees, which is the RelNode Eval_Spec result.
  //   Tv == dbof(VisibleAt(Ws, ts)): the statement's transaction has no
  //   captured dependency writes (HitAllowed requires it), so it sees exactly
  //   the committed dependency state of its snapshot (TCB pg_mvcc).
  lemma Runtime_hit_correct(Ws: set<Writer>, D: set<Slot>, dbof: set<Writer> -> TDB,
                            M: Marker, R: set<Writer>, s: PStore,
                            S: PgSnap, ts: int, ti: map<Slot, int>, tq: map<Slot, int>,
                            nn: string, Tv: TDB,
                            ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es) && History(Ws) && DbOf(Ws, D, dbof) && SInv(Ws, D, dbof, M, R, s)
    requires SnapAt(Ws, S, ts) && Reads(D, ts, ti, tq) && (forall k :: k in D ==> M.tau[k] <= tq[k])
    requires HitCheck(Ws, D, M, S, ti, tq)
    requires Tv == dbof(VisibleAt(Ws, ts))
    ensures RuntimeExecute(true, s, nn, Tv, ei, es) == SQLRows(Tv, nn)
    ensures Eval_Spec(Q21RelNodeData.LogicalSpec_Q21_rel, Q21Params(nn), ToRel(Tv))
            == SomeRows(Agg.PairRows(Agg.AnswerPairs(Tv, nn)))
    ensures ToOuts(Agg.AnswerPairs(Tv, nn)) == SQLRows(Tv, nn)
  {
    Agg.Q21_EvalSpec_Answer(Tv, nn);
    Hit_sound(Ws, D, M, R, S, ts, ti, tq);
    var V := VisibleAt(Ws, ts);
    assert dbof(V) == dbof(V * Rel(Ws, D));
    assert Tv == dbof(R);
    var r := Execute(ArgText(nn), false, s.PValid?, Snap(s), ei);
    if r.Emitted? {
      assert s.PValid?;
      Execute_correct(Tv, nn, s.m, ei);
    } else {
      ExecuteFallback_correct(Tv, nn, ei, es);
    }
  }
}
