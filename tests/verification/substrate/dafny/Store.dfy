// Structural twin of PartitionedStateStore
// SOURCE: src/stateful/state/partitioned_state_store.cpp
//   PublishPartition, TryPublishPartition, InvalidatePartition, GetStatus,
//   GetReadablePayload, GetPartitionVersion
// Mutex / shared_ptr identity abstracted; payload = ghost int id.

module SubstrateStore {

  datatype Status = Absent | Invalid | Valid

  datatype Slot = Slot(
    status: Status,
    payload: int,           // 0 = null
    partition_version: nat,
    valid_through_mutation_epoch: nat
  )

  ghost function EmptySlot(): Slot {
    Slot(Absent, 0, 0, 0)
  }

  type Store = map<int, Slot>  // PartitionKey -> Slot

  ghost function GetStatus(store: Store, key: int): Status {
    if key !in store then Absent else store[key].status
  }

  ghost function GetReadablePayload(store: Store, key: int): int {
    if key !in store || store[key].status != Valid then 0
    else store[key].payload
  }

  ghost function GetPartitionVersion(store: Store, key: int): nat {
    if key !in store then 0 else store[key].partition_version
  }

  // PublishPartition (partitioned_state_store.cpp:~62)
  ghost function PublishPartition(store: Store, key: int, payload: int, epoch: nat): Store {
    var before := if key in store then store[key] else EmptySlot();
    store[key := Slot(Valid, payload, before.partition_version + 1, epoch)]
  }

  // TryPublishPartition (partitioned_state_store.cpp:~74)
  ghost function TryPublishPartition(
    store: Store, key: int, payload: int, epoch: nat, expected_version: nat
  ): (Store, bool) {
    var current_version := GetPartitionVersion(store, key);
    if expected_version != 0 && current_version != expected_version then
      var before := if key in store then store[key] else EmptySlot();
      (store[key := Slot(Invalid, 0, before.partition_version + 1,
                         before.valid_through_mutation_epoch)], false)
    else
      var before := if key in store then store[key] else EmptySlot();
      (store[key := Slot(Valid, payload, before.partition_version + 1, epoch)], true)
  }

  ghost function InvalidatePartition(store: Store, key: int): Store {
    var before := if key in store then store[key] else EmptySlot();
    store[key := Slot(Invalid, 0, before.partition_version + 1, 0)]
  }

  lemma PublishThenReadable(store: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures var s2 := PublishPartition(store, key, payload, epoch);
            GetStatus(s2, key) == Valid &&
            GetReadablePayload(s2, key) == payload
  {
  }

  lemma TryPublishSuccessReadable(store: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures var r := TryPublishPartition(store, key, payload, epoch, 0);
            r.1 && GetStatus(r.0, key) == Valid &&
            GetReadablePayload(r.0, key) == payload
  {
  }

  lemma TryPublishCasFailInvalidates(store: Store, key: int, payload: int, epoch: nat, expected: nat)
    requires expected != 0
    requires GetPartitionVersion(store, key) != expected
    ensures var r := TryPublishPartition(store, key, payload, epoch, expected);
            !r.1 && GetStatus(r.0, key) == Invalid &&
            GetReadablePayload(r.0, key) == 0
  {
  }

  lemma InvalidateClearsReadable(store: Store, key: int)
    ensures var s2 := InvalidatePartition(store, key);
            GetStatus(s2, key) == Invalid &&
            GetReadablePayload(s2, key) == 0
  {
  }

  lemma PublishBumpsVersion(store: Store, key: int, payload: int, epoch: nat)
    ensures GetPartitionVersion(PublishPartition(store, key, payload, epoch), key)
         == GetPartitionVersion(store, key) + 1
  {
  }
}
