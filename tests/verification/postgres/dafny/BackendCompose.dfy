// Tier D: composition lemmas discharging Q1ModuleTwin store/scan/validity oracles
// under substrate + postgres backend twins.
//
// SQL ≡ aux remains differential (tests/aux_state/test_postgres_tpch_differential.cpp).
// TCB: PostgreSQL SPI, xact callbacks, heap AM.

include "../../substrate/dafny/Coordinator.dfy"
include "../../q1/dafny/Q1ModuleTwin.dfy"

module BackendCompose {
  import opened SubstrateStore
  import opened SubstrateCoordinator
  import opened Q1ModuleTwin
  import opened Q1Finalize

  // After Build+Install from scan stream, store partitions are the installed images.
  lemma BuildInstallReadable(store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures GetReadablePayload(PublishPartition(store0, key, payload, epoch), key) == payload
  {
    PublishThenReadable(store0, key, payload, epoch);
  }

  // Mutation+commit ApplyDelta path: staged publish matches ReadPartitions payload.
  lemma MutationCommitApplyDeltaReadable(store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures var outcome := StagedOutcome(true, key, payload, true, false);
            var c2 := CommitApplyDelta(PreCommit(
              CoordState(Mutating, store0, 1, false, outcome), outcome), epoch);
            GetStatus(c2.store, key) == Valid &&
            GetReadablePayload(c2.store, key) == payload
  {
    StagedApplyDeltaThenCommit(store0, key, payload, epoch);
  }

  // Execute ≡ Fallback when store Inv holds (discharges validity+load oracles
  // when store_days == built from same scan).
  lemma ExecuteEqualsFallbackUnderStoreInv(
    args: QueryArgs,
    base_days: int,
    days: DayStore
  )
    requires args.has_offset
    ensures Execute(args, base_days, true, true, days, days)
         == ExecuteFallback(args, base_days, days)
  {
    ExecuteEqualsFallbackWhenInv(args, base_days, days, days);
  }

  // Invalid store ⇒ Execute uses fallback build (CheckValidity oracle false).
  lemma ExecuteFallsBackWhenStoreInvalid(
    args: QueryArgs,
    base_days: int,
    store_days: DayStore,
    built: DayStore
  )
    requires args.has_offset
    ensures Execute(args, base_days, false, true, store_days, built)
         == ExecuteFallback(args, base_days, built)
  {
    ExecuteFallsBackWhenInvalid(args, base_days, store_days, built);
  }

  // Load failure ⇒ same fallback.
  lemma ExecuteFallsBackWhenLoadFailsCompose(
    args: QueryArgs,
    base_days: int,
    store_days: DayStore,
    built: DayStore
  )
    requires args.has_offset
    ensures Execute(args, base_days, true, false, store_days, built)
         == ExecuteFallback(args, base_days, built)
  {
    ExecuteFallsBackWhenLoadFails(args, base_days, store_days, built);
  }
}
