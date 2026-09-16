// Structural twin of SelectAuxMaintenanceAction (forced + zero-row + eligibility)
// SOURCE: src/stateful/core/aux_maintenance_policy.cpp
//         src/stateful/maintenance/action_selector.cpp (SelectMinCostAction core)
// Float cost arithmetic twin'd with int costs for induction; conformance binds
// real SelectAuxMaintenanceAction on concrete requests.

module SubstrateMaintenance {

  datatype Option<T> = None | Some(value: T)

  datatype MaintenanceAction =
    NoOp | ApplyDelta | RebuildPartition | RebuildAll | InvalidateAndFallback

  datatype MaintenanceRequest = MaintenanceRequest(
    rows_touched: nat,
    affected_partition_count: nat,
    incremental_supported: bool,
    partition_rebuild_supported: bool,
    full_rebuild_supported: bool,
    whole_state_required: bool,
    apply_delta_max_rows: nat
  )

  // ParseMaintenanceActionName (aux_maintenance_policy.cpp:~38)
  ghost function ParseMaintenanceActionName(name: string): Option<MaintenanceAction> {
    if name == "ApplyDelta" then Some(ApplyDelta)
    else if name == "RebuildAll" then Some(RebuildAll)
    else if name == "RebuildPartition" then Some(RebuildPartition)
    else if name == "InvalidateAndFallback" then Some(InvalidateAndFallback)
    else if name == "NoOp" then Some(NoOp)
    else if name == "" then None
    else None
  }

  // SelectAuxMaintenanceAction early control flow (cpp:~110–124)
  ghost function SelectAuxMaintenanceActionEarly(
    forced: Option<MaintenanceAction>,
    request: MaintenanceRequest
  ): Option<MaintenanceAction> {
    if forced.Some? then
      Some(forced.value)
    else if request.rows_touched == 0 && !request.whole_state_required then
      Some(NoOp)
    else
      None
  }

  lemma ForcedWins(forced: MaintenanceAction, request: MaintenanceRequest)
    ensures SelectAuxMaintenanceActionEarly(Some(forced), request) == Some(forced)
  {
  }

  lemma ZeroRowsNoOp(request: MaintenanceRequest)
    requires request.rows_touched == 0
    requires !request.whole_state_required
    ensures SelectAuxMaintenanceActionEarly(None, request) == Some(NoOp)
  {
  }

  ghost function ApplyDeltaEligible(request: MaintenanceRequest): bool {
    request.incremental_supported &&
    !request.whole_state_required &&
    request.rows_touched <= request.apply_delta_max_rows
  }

  datatype Candidate = Candidate(action: MaintenanceAction, cost: int, supported: bool)

  ghost function MinSupported(cands: seq<Candidate>): MaintenanceAction
    decreases |cands|
  {
    MinSupportedRec(cands, None)
  }

  ghost function MinSupportedRec(cands: seq<Candidate>, best: Option<(MaintenanceAction, int)>): MaintenanceAction
    decreases |cands|
  {
    if |cands| == 0 then
      if best.None? then InvalidateAndFallback else best.value.0
    else if !cands[0].supported then
      MinSupportedRec(cands[1..], best)
    else if best.None? || cands[0].cost < best.value.1 then
      MinSupportedRec(cands[1..], Some((cands[0].action, cands[0].cost)))
    else
      MinSupportedRec(cands[1..], best)
  }

  lemma EmptyCandidatesInvalidate()
    ensures MinSupported([]) == InvalidateAndFallback
  {
  }

  lemma SoleSupportedWins(a: MaintenanceAction, cost: int)
    ensures MinSupported([Candidate(a, cost, true)]) == a
  {
    assert MinSupportedRec([Candidate(a, cost, true)], None)
        == MinSupportedRec([], Some((a, cost)));
    assert MinSupportedRec([], Some((a, cost))) == a;
  }

  lemma ParseKnownNames()
    ensures ParseMaintenanceActionName("ApplyDelta") == Some(ApplyDelta)
    ensures ParseMaintenanceActionName("NoOp") == Some(NoOp)
    ensures ParseMaintenanceActionName("bogus") == None
  {
  }
}
