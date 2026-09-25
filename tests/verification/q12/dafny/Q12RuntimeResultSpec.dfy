// Q12RuntimeResultSpec — Hit → Inv → Exec == EvalResultSpec.
// Reuses SnapshotPinning substrate; does not redesign snapshot semantics.

include "Q12Aux.dfy"
include "Q12ResultSpecBridge.dfy"
include "../../substrate/dafny/SnapshotPinning.dfy"

module Q12RuntimeResultSpec {

  import opened Q12Aux
  import Bridge = Q12ResultSpecBridge
  import Spec = Q12ResultSpec
  import SP = SnapshotPinning

  // Model-level Hit: published state matches DB snapshot (no pending dirty).
  ghost predicate AcceleratorHit_Q12(pending: bool, read_complete: bool) {
    !pending && read_complete
  }

  ghost predicate PairInv_Q12(s: State, orders: seq<OrderRow>, lines: seq<LineRow>) {
    Inv(s, orders, lines)
  }

  ghost predicate SnapshotHitSafe_Q12(
    hit: bool,
    orders_t: seq<OrderRow>, lines_t: seq<LineRow>,
    orders_S: seq<OrderRow>, lines_S: seq<LineRow>
  ) {
    hit ==> (orders_t == orders_S && lines_t == lines_S)
  }

  lemma SnapshotInv(
    s: State,
    orders_t: seq<OrderRow>, lines_t: seq<LineRow>,
    orders_S: seq<OrderRow>, lines_S: seq<LineRow>,
    pending: bool,
    read_complete: bool
  )
    requires AcceleratorHit_Q12(pending, read_complete)
    requires PairInv_Q12(s, orders_t, lines_t)
    requires SnapshotHitSafe_Q12(true, orders_t, lines_t, orders_S, lines_S)
    ensures Inv(s, orders_S, lines_S)
  {
  }

  lemma Hit_implies_ExecExactResultSpec(
    s: State,
    orders_t: seq<OrderRow>, lines_t: seq<LineRow>,
    orders_S: seq<OrderRow>, lines_S: seq<LineRow>,
    pending: bool,
    read_complete: bool,
    start: int, endExclusive: int, m1: string, m2: string
  )
    requires AcceleratorHit_Q12(pending, read_complete)
    requires PairInv_Q12(s, orders_t, lines_t)
    requires SnapshotHitSafe_Q12(true, orders_t, lines_t, orders_S, lines_S)
    ensures Bridge.ExactEq(
              ExecSeq(orders_S, lines_S, start, endExclusive, m1, m2),
              Spec.EvalResultSpec(Bridge.AuxOrdersToSpec(orders_S),
                                  Bridge.AuxLinesToSpec(lines_S),
                                  start, endExclusive, m1, m2))
  {
    SnapshotInv(s, orders_t, lines_t, orders_S, lines_S, pending, read_complete);
    Bridge.Q12ExecCorrect(s, orders_S, lines_S, start, endExclusive, m1, m2);
  }

  // TokenForSnapshot reused from SnapshotPinning (unchanged).
  lemma TokenPin_reused(S: SP.PgSnapshot, epoch: nat)
    ensures SP.TokenForSnapshot(S, SP.MakeTokenForSnapshot(S, epoch))
  {
    SP.MakeToken_establishes_TokenForSnapshot(S, epoch);
  }
}
