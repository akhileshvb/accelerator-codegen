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

  // ===================== PostgreSQL snapshots (content) =====================

  // aux_sync::MarkOf / the statement snapshot: xmax and xip ∪ subxip.
  datatype PgSnap = PgSnap(xmax: nat, running: set<nat>)

  // XidInMVCCSnapshot for a non-overflowed MVCC snapshot (running ⊆ [xmin, xmax)).
  predicate InSnap(S: PgSnap, x: nat) { x >= S.xmax || x in S.running }

  // aux_sync::Covers.
  predicate Covers(S: PgSnap, M: PgSnap)
  { S.xmax >= M.xmax && forall y :: y in S.running && y < M.xmax ==> y in M.running }

  lemma Covers_visible(S: PgSnap, M: PgSnap, x: nat)
    requires Covers(S, M) && !InSnap(M, x)
    ensures !InSnap(S, x)
  {}

  // ===================== Writer history =====================

  type Slot = nat

  datatype Writer = Writer(xid: nat, slots: set<Slot>, committed: bool,
                           a: map<Slot, int>, v: int, s: map<Slot, int>, d: map<Slot, int>)

  ghost predicate WProto(W: Writer)
  {
    W.slots <= W.a.Keys && W.slots <= W.d.Keys && (W.committed ==> W.slots <= W.s.Keys) &&
    forall k :: k in W.slots ==>
      W.a[k] < W.d[k] && (W.committed ==> W.a[k] < W.v < W.s[k] < W.d[k])
  }

  ghost predicate History(Ws: set<Writer>)
  {
    (forall W :: W in Ws ==> WProto(W)) &&
    (forall W1, W2 :: W1 in Ws && W2 in Ws && W1 != W2 ==> W1.xid != W2.xid) &&
    (forall W1, W2, k :: (W1 in Ws && W2 in Ws && W1 != W2 && W1.committed && W2.committed &&
                          k in W1.slots && k in W2.slots) ==> W1.s[k] != W2.s[k])
  }

  ghost function Inflight(Ws: set<Writer>, k: Slot, t: int): set<Writer>
  { set W | W in Ws && k in W.slots && k in W.a && k in W.d && W.a[k] <= t < W.d[k] }

  ghost function Counted(Ws: set<Writer>, k: Slot, t: int): set<Writer>
  { set W | W in Ws && W.committed && k in W.slots && k in W.s && W.s[k] <= t }

  ghost function VisibleAt(Ws: set<Writer>, ts: int): set<Writer>
  { set W | W in Ws && W.committed && W.v < ts }

  ghost function Rel(Ws: set<Writer>, D: set<Slot>): set<Writer>
  { set W | W in Ws && W.slots * D != {} }

  ghost function Proj(R: set<Writer>, k: Slot): set<Writer>
  { set W | W in R && k in W.slots }

  // PostgreSQL MVCC (TCB pg_mvcc): a committed writer is visible to the
  // snapshot taken at time ts iff its commit became visible before ts.
  ghost predicate SnapAt(Ws: set<Writer>, S: PgSnap, ts: int)
  { forall W :: W in Ws && W.committed ==> (!InSnap(S, W.xid) <==> W.v < ts) }

  lemma CardSubset<T>(A: set<T>, B: set<T>)
    requires A <= B
    ensures |A| <= |B|
    decreases B
  {
    if A != {} {
      var x :| x in A;
      CardSubset(A - {x}, B - {x});
    }
  }

  lemma SubsetCardEq<T>(A: set<T>, B: set<T>)
    requires A <= B && |A| == |B|
    ensures A == B
  {
    if A != B {
      var x :| x in B && x !in A;
      CardSubset(A, B - {x});
    }
  }

  // Equal seq reads at t1 <= t2 => nothing was counted in between.
  lemma Counted_stable(Ws: set<Writer>, k: Slot, t1: int, t2: int)
    requires t1 <= t2 && |Counted(Ws, k, t1)| == |Counted(Ws, k, t2)|
    ensures Counted(Ws, k, t1) == Counted(Ws, k, t2)
  { SubsetCardEq(Counted(Ws, k, t1), Counted(Ws, k, t2)); }

  // ===================== Sync marker =====================

  // base: build snapshot; own: xids of this backend's maintained commits;
  // seq[k]: recorded seq value; tau[k]: a time at which it was exact (ghost).
  datatype Marker = Marker(base: PgSnap, own: set<nat>, cnt: map<Slot, nat>, tau: map<Slot, int>)

  // The marker describes R: the dependency writers the store reflects.
  ghost predicate MInv(Ws: set<Writer>, D: set<Slot>, M: Marker, R: set<Writer>)
  {
    R <= Rel(Ws, D) && (forall W :: W in R ==> W.committed) &&
    D <= M.cnt.Keys && D <= M.tau.Keys &&
    (forall k :: k in D ==> Counted(Ws, k, M.tau[k]) == Proj(R, k) && |Proj(R, k)| == M.cnt[k]) &&
    (forall W :: W in R ==> !InSnap(M.base, W.xid) || W.xid in M.own)
  }

  // aux_sync::ReadCounters: inflight[k] read at ti[k], then (after a barrier)
  // seq[k] read at tq[k] >= ti[k]; all reads after `after`.
  ghost predicate Reads(D: set<Slot>, after: int, ti: map<Slot, int>, tq: map<Slot, int>)
  { D <= ti.Keys && D <= tq.Keys && forall k :: k in D ==> after < ti[k] <= tq[k] }

  // Runtime::HitAllowed (counter part read after the statement snapshot).
  ghost predicate HitCheck(Ws: set<Writer>, D: set<Slot>, M: Marker, S: PgSnap,
                           ti: map<Slot, int>, tq: map<Slot, int>)
    requires D <= ti.Keys && D <= tq.Keys
  {
    D <= M.cnt.Keys &&
    Covers(S, M.base) && (forall x :: x in M.own ==> !InSnap(S, x)) &&
    forall k :: k in D ==> Inflight(Ws, k, ti[k]) == {} && |Counted(Ws, k, tq[k])| == M.cnt[k]
  }

  lemma {:isolate_assertions} Hit_sound(Ws: set<Writer>, D: set<Slot>, M: Marker, R: set<Writer>,
                                        S: PgSnap, ts: int, ti: map<Slot, int>, tq: map<Slot, int>)
    requires History(Ws) && MInv(Ws, D, M, R) && SnapAt(Ws, S, ts) && Reads(D, ts, ti, tq)
    requires forall k :: k in D ==> M.tau[k] <= tq[k]
    requires HitCheck(Ws, D, M, S, ti, tq)
    ensures VisibleAt(Ws, ts) * Rel(Ws, D) == R
  {
    forall k | k in D ensures Counted(Ws, k, tq[k]) == Proj(R, k) {
      Counted_stable(Ws, k, M.tau[k], tq[k]);
    }
    forall W | W in VisibleAt(Ws, ts) * Rel(Ws, D) ensures W in R {
      var k :| k in W.slots * D;
      assert WProto(W);
      assert W !in Inflight(Ws, k, ti[k]);
      assert W in Counted(Ws, k, tq[k]);
    }
    forall W | W in R ensures W in VisibleAt(Ws, ts) * Rel(Ws, D) {
      if !InSnap(M.base, W.xid) { Covers_visible(S, M.base, W.xid); }
    }
  }

  ghost function BuildMarker(Sb: PgSnap, c: map<Slot, nat>, D: set<Slot>, t0: int): Marker
  { Marker(Sb, {}, c, map k | k in D :: t0) }

  // Runtime::Rebuild: ReadCounters at t0, fresh snapshot at ts_b, Build,
  // ReadCounters at t1 (inflight 0 and seq unchanged).
  lemma {:isolate_assertions} Build_sound(Ws: set<Writer>, D: set<Slot>, Sb: PgSnap,
                                          t0: int, tsb: int, ti: map<Slot, int>, tq: map<Slot, int>,
                                          c: map<Slot, nat>)
    requires History(Ws) && SnapAt(Ws, Sb, tsb) && t0 < tsb && Reads(D, tsb, ti, tq)
    requires D <= c.Keys
    requires forall k :: k in D ==> |Counted(Ws, k, t0)| == c[k]
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) == {} && |Counted(Ws, k, tq[k])| == c[k]
    ensures MInv(Ws, D, BuildMarker(Sb, c, D, t0), VisibleAt(Ws, tsb) * Rel(Ws, D))
  {
    var R := VisibleAt(Ws, tsb) * Rel(Ws, D);
    var M := BuildMarker(Sb, c, D, t0);
    forall k | k in D ensures Counted(Ws, k, t0) == Proj(R, k) && |Proj(R, k)| == c[k] {
      Counted_stable(Ws, k, t0, tq[k]);
      forall W | W in Counted(Ws, k, t0) ensures W in Proj(R, k) {
        assert WProto(W);
        assert W.v < W.s[k] <= t0 < tsb;
        assert W in VisibleAt(Ws, tsb);
        assert k in W.slots * D;
        assert W in Rel(Ws, D);
      }
      forall W | W in Proj(R, k) ensures W in Counted(Ws, k, t0) {
        assert WProto(W);
        assert W !in Inflight(Ws, k, ti[k]);
        assert W in Counted(Ws, k, tq[k]);
      }
    }
    assert M.tau == map k | k in D :: t0;
  }

  ghost function OwnSeq(M: Marker, D: set<Slot>, A: Writer): map<Slot, nat>
    requires D <= M.cnt.Keys
  { map k | k in D :: if k in A.slots then M.cnt[k] + 1 else M.cnt[k] }

  ghost function OwnTau(M: Marker, D: set<Slot>, A: Writer): map<Slot, int>
    requires D <= M.tau.Keys && (A.committed ==> A.slots <= A.s.Keys)
  { map k | k in D :: if k in A.slots && A.committed then A.s[k] else M.tau[k] }

  // The backend's own commit of A (runtime.cpp BeforePreCommit / AfterCommit):
  //   PRE_COMMIT at tp (after all of A's writes, maintenance snapshot pushed):
  //     inflight[k] is A alone (or empty) and seq[k] == marker.cnt[k];
  //   COMMIT: fetch_add on each written dependency slot returned marker.cnt[k].
  lemma {:isolate_assertions} OwnCommit_sound(Ws: set<Writer>, D: set<Slot>, M: Marker, R: set<Writer>,
                                              A: Writer, tp: int, ti: map<Slot, int>, tq: map<Slot, int>)
    requires History(Ws) && MInv(Ws, D, M, R) && A in Ws && A.committed
    requires forall k :: k in D ==> M.tau[k] <= tp
    requires forall k :: k in A.slots ==> A.a[k] < tp
    requires Reads(D, tp, ti, tq) && forall k :: k in D ==> tq[k] < A.v
    requires forall k :: k in D ==> Inflight(Ws, k, ti[k]) <= {A} && |Counted(Ws, k, tq[k])| == M.cnt[k]
    requires forall k :: k in D * A.slots ==> |Counted(Ws, k, A.s[k] - 1)| == M.cnt[k]
    ensures VisibleAt(Ws, tp) * Rel(Ws, D) == R
    ensures MInv(Ws, D, Marker(M.base, M.own + {A.xid}, OwnSeq(M, D, A), OwnTau(M, D, A)),
                 if A.slots * D != {} then R + {A} else R)
  {
    assert WProto(A);
    forall k | k in D ensures Counted(Ws, k, tq[k]) == Proj(R, k) {
      Counted_stable(Ws, k, M.tau[k], tq[k]);
    }
    forall W | W in VisibleAt(Ws, tp) * Rel(Ws, D) ensures W in R {
      var k :| k in W.slots * D;
      assert WProto(W);
      assert W != A;
      assert W !in Inflight(Ws, k, ti[k]);
      assert W in Counted(Ws, k, tq[k]);
    }
    forall W | W in R ensures W in VisibleAt(Ws, tp) {
      var k :| k in W.slots * D;
      assert W in Proj(R, k);
      assert WProto(W);
    }
    var R2 := if A.slots * D != {} then R + {A} else R;
    var M2 := Marker(M.base, M.own + {A.xid}, OwnSeq(M, D, A), OwnTau(M, D, A));
    forall k | k in D ensures Counted(Ws, k, M2.tau[k]) == Proj(R2, k) && |Proj(R2, k)| == M2.cnt[k] {
      assert tp < A.v by { assert tp < ti[k] <= tq[k] < A.v; }
      OwnCommit_slot(Ws, D, M, R, A, tp, k);
    }
  }

  lemma {:isolate_assertions} OwnCommit_slot(Ws: set<Writer>, D: set<Slot>, M: Marker, R: set<Writer>,
                                             A: Writer, tp: int, k: Slot)
    requires History(Ws) && MInv(Ws, D, M, R) && A in Ws && A.committed && k in D
    requires M.tau[k] <= tp && tp < A.v
    requires k in A.slots ==> |Counted(Ws, k, A.s[k] - 1)| == M.cnt[k]
    ensures var R2 := if A.slots * D != {} then R + {A} else R;
            var M2 := Marker(M.base, M.own + {A.xid}, OwnSeq(M, D, A), OwnTau(M, D, A));
            Counted(Ws, k, M2.tau[k]) == Proj(R2, k) && |Proj(R2, k)| == M2.cnt[k]
  {
    assert WProto(A);
    var R2 := if A.slots * D != {} then R + {A} else R;
    if k in A.slots {
      assert A.slots * D != {} by { assert k in A.slots * D; }
      assert M.tau[k] <= A.s[k] - 1;
      Counted_stable(Ws, k, M.tau[k], A.s[k] - 1);
      assert A !in Counted(Ws, k, A.s[k] - 1);
      var C1 := Counted(Ws, k, A.s[k] - 1);
      var C2 := Counted(Ws, k, A.s[k]);
      forall W | W in C2 && W != A ensures W in C1 {
        assert W.s[k] != A.s[k];
      }
      assert C2 == C1 + {A};
      assert Proj(R2, k) == Proj(R, k) + {A};
      assert A !in Proj(R, k);
    } else {
      assert Proj(R2, k) == Proj(R, k);
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
  { MInv(Ws, D, M, R) && RuntimeInv(s, dbof(R)) }

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
