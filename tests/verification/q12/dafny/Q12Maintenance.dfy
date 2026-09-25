// Q12Maintenance — Inv under Rebuild + lineitem ApplyDelta + order ReclassifyOrder.
//
// Order ApplyDelta mirrors C++ reverse-index reclassify (ReclassifyOrder).
// Inv proof uses ReclassifyOrder_eq_BuildFold (AdjustDays ≡ FoldLines residual).

include "Q12Aux.dfy"

module Q12Maintenance {
  import opened Q12Aux

  datatype MaintAction =
    | NoOp
    | ApplyLineInsert(l: LineRow)
    | ApplyLineDeleteLast(l: LineRow)
    | RebuildAll
    | InvalidateAndFallback
    | ApplyOrderInsert(o: OrderRow)
    | ApplyOrderDeleteLast(o: OrderRow)
    | ApplyOrderUpdate(oOld: OrderRow, oNew: OrderRow)

  datatype AuxState = AuxState(
    state: State,
    orders: seq<OrderRow>,
    lines: seq<LineRow>,
    valid: bool
  )

  ghost predicate StateInv(s: AuxState) {
    Inv(s.state, s.orders, s.lines)
  }

  ghost predicate UniqueOrderKeys(orders: seq<OrderRow>) {
    forall i, j :: 0 <= i < j < |orders| ==> orders[i].orderkey != orders[j].orderkey
  }

  ghost predicate ActionOk(s: AuxState, a: MaintAction) {
    match a
      case NoOp => true
      case ApplyLineInsert(l) => true
      case ApplyLineDeleteLast(l) =>
        |s.lines| > 0 && s.lines[|s.lines| - 1] == l
      case RebuildAll => true
      case InvalidateAndFallback => true
      case ApplyOrderInsert(o) =>
        forall i :: 0 <= i < |s.orders| ==> s.orders[i].orderkey != o.orderkey
      case ApplyOrderDeleteLast(o) =>
        |s.orders| > 0 && s.orders[|s.orders| - 1] == o &&
        forall i :: 0 <= i < |s.orders| - 1 ==> s.orders[i].orderkey != o.orderkey
      case ApplyOrderUpdate(oOld, oNew) =>
        |s.orders| > 0 && s.orders[|s.orders| - 1] == oOld &&
        oOld.orderkey == oNew.orderkey &&
        forall i :: 0 <= i < |s.orders| - 1 ==> s.orders[i].orderkey != oOld.orderkey
  }

  ghost function ApplyMaint(s: AuxState, a: MaintAction): AuxState
    requires ActionOk(s, a)
  {
    match a
      case NoOp => s
      case ApplyLineInsert(l) =>
        AuxState(State(s.state.priorities,
                       ApplyLineDelta(s.state.days, s.state.priorities, l, 1)),
                 s.orders, s.lines + [l], s.valid)
      case ApplyLineDeleteLast(l) =>
        AuxState(State(s.state.priorities,
                       ApplyLineDelta(s.state.days, s.state.priorities, l, -1)),
                 s.orders, s.lines[..|s.lines| - 1], s.valid)
      case RebuildAll =>
        AuxState(BuildFold(s.orders, s.lines), s.orders, s.lines, s.valid)
      case InvalidateAndFallback =>
        AuxState(s.state, s.orders, s.lines, false)
      case ApplyOrderInsert(o) =>
        AuxState(ReclassifyOrder(s.state, s.lines, o.orderkey, ClassifyPriority(o.priority)),
                 s.orders + [o], s.lines, s.valid)
      case ApplyOrderDeleteLast(o) =>
        AuxState(ReclassifyOrder(s.state, s.lines, o.orderkey, Absent),
                 s.orders[..|s.orders| - 1], s.lines, s.valid)
      case ApplyOrderUpdate(oOld, oNew) =>
        AuxState(ReclassifyOrder(s.state, s.lines, oNew.orderkey, ClassifyPriority(oNew.priority)),
                 s.orders[..|s.orders| - 1] + [oNew], s.lines, s.valid)
  }

  lemma MaintNoOp(s: AuxState)
    requires StateInv(s)
    ensures StateInv(ApplyMaint(s, NoOp))
  {
  }

  lemma MaintLineInsert(s: AuxState, l: LineRow)
    requires StateInv(s)
    ensures StateInv(ApplyMaint(s, ApplyLineInsert(l)))
  {
    InvPreservedByLineInsert(s.state, s.orders, s.lines, l);
  }

  lemma MaintLineDeleteLast(s: AuxState, l: LineRow)
    requires StateInv(s) && ActionOk(s, ApplyLineDeleteLast(l))
    ensures StateInv(ApplyMaint(s, ApplyLineDeleteLast(l)))
  {
    InvPreservedByLineDeleteLast(s.state, s.orders, s.lines, l);
  }

  lemma MaintRebuild(s: AuxState)
    ensures StateInv(ApplyMaint(s, RebuildAll))
  {
    RebuildEstablishesInv(s.orders, s.lines);
  }

  lemma MaintInvalidate(s: AuxState)
    requires StateInv(s)
    ensures var s2 := ApplyMaint(s, InvalidateAndFallback);
            !s2.valid && Inv(s2.state, s2.orders, s2.lines)
  {
  }

  lemma MaintOrderInsert(s: AuxState, o: OrderRow)
    requires StateInv(s) && ActionOk(s, ApplyOrderInsert(o))
    ensures StateInv(ApplyMaint(s, ApplyOrderInsert(o)))
  {
    InvPreservedByOrderInsert(s.state, s.orders, s.lines, o);
  }

  lemma MaintOrderDeleteLast(s: AuxState, o: OrderRow)
    requires StateInv(s) && ActionOk(s, ApplyOrderDeleteLast(o))
    ensures StateInv(ApplyMaint(s, ApplyOrderDeleteLast(o)))
  {
    InvPreservedByOrderDeleteLast(s.state, s.orders, s.lines, o);
  }

  lemma MaintOrderUpdate(s: AuxState, oOld: OrderRow, oNew: OrderRow)
    requires StateInv(s) && ActionOk(s, ApplyOrderUpdate(oOld, oNew))
    ensures StateInv(ApplyMaint(s, ApplyOrderUpdate(oOld, oNew)))
  {
    InvPreservedByOrderUpdate(s.state, s.orders, s.lines, oOld, oNew);
  }

  lemma MaintenancePreservesInv(s: AuxState, a: MaintAction)
    requires StateInv(s) && ActionOk(s, a)
    ensures var s2 := ApplyMaint(s, a);
            Inv(s2.state, s2.orders, s2.lines) &&
            (a.InvalidateAndFallback? ==> !s2.valid) &&
            (!a.InvalidateAndFallback? ==> s2.valid == s.valid)
  {
    match a
      case NoOp => MaintNoOp(s);
      case ApplyLineInsert(l) => MaintLineInsert(s, l);
      case ApplyLineDeleteLast(l) => MaintLineDeleteLast(s, l);
      case RebuildAll => MaintRebuild(s);
      case InvalidateAndFallback => MaintInvalidate(s);
      case ApplyOrderInsert(o) => MaintOrderInsert(s, o);
      case ApplyOrderDeleteLast(o) => MaintOrderDeleteLast(s, o);
      case ApplyOrderUpdate(oOld, oNew) => MaintOrderUpdate(s, oOld, oNew);
  }

  lemma MaintTwoStepPreservesInv(s: AuxState, a1: MaintAction, a2: MaintAction)
    requires StateInv(s)
    requires ActionOk(s, a1)
    requires ActionOk(ApplyMaint(s, a1), a2)
    ensures StateInv(ApplyMaint(ApplyMaint(s, a1), a2))
  {
    MaintenancePreservesInv(s, a1);
    var s1 := ApplyMaint(s, a1);
    MaintenancePreservesInv(s1, a2);
  }

  lemma InvalidateThenRebuild(s: AuxState)
    requires StateInv(s)
    ensures var s1 := ApplyMaint(s, InvalidateAndFallback);
            var s2 := ApplyMaint(s1, RebuildAll);
            !s1.valid && StateInv(s2)
  {
    MaintInvalidate(s);
    var s1 := ApplyMaint(s, InvalidateAndFallback);
    MaintRebuild(s1);
  }
}
