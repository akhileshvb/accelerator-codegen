// SCAFFOLDING
// C3: per-accelerator ordering, gap detection and catch-up of committed changes.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "substrate/aux/runtime/columnar_batch.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"

namespace accelerator_codegen::aux_state::runtime {

// One committed change of one accelerator: all its batches, in statement
// order (subtransaction aborts already removed).
struct CommittedChange {
  Seq seq = 0;
  Epoch epoch = 0;
  std::vector<ColumnarBatchRef> batches;
  bool empty_commit = false; // aborted / no-op: only advances the sequence
  // The change cannot be maintained incrementally (e.g. TRUNCATE): applying
  // it leaves the state stale, so it is rebuilt.
  bool invalidates = false;
};

// Pure bookkeeping; no locks (the owning AcceleratorRuntime serializes calls).
//
// States:
//   Synced(seq s)   - the installed state includes exactly commits 1..s.
//   Building(b)     - a build from a snapshot at epoch b is running; commits
//                     arriving meanwhile are queued.
//   Stale           - the state is invalid and no build runs; commits are
//                     queued until a build is started.
//
// Every committed change is Offer()ed in any order; TakeReady() returns the
// changes that can be applied now, in sequence order.
class SequencedApplier {
public:
  enum class Mode { Synced, Building, Stale };

  // Starts Stale with nothing applied.
  SequencedApplier() = default;

  Mode CurrentMode() const { return mode; }
  // Highest sequence number whose commit is reflected in the state (Synced).
  Seq AppliedSeq() const { return applied_seq; }
  Epoch AppliedEpoch() const { return applied_epoch; }
  // Highest sequence number seen so far (applied, queued or announced).
  Seq HighestSeenSeq() const { return highest_seen; }

  // A commit arrived (any order). Returns false if `change.seq` was already
  // applied or offered (duplicate; ignored).
  bool Offer(CommittedChange change);
  // The commit with `seq` is known to exist (e.g. its sequence number was
  // assigned) even though its change has not arrived yet: readers needing it
  // must wait for it, and a missing change is detectable as a gap.
  void Announce(Seq seq);

  // Synced mode: the next changes to apply, consecutive from AppliedSeq()+1,
  // removed from the queue. Empty when the next one has not arrived (a gap).
  std::vector<CommittedChange> TakeReady();
  // Records that `change` was applied to the state.
  void MarkApplied(const CommittedChange &change);

  // A gap: the change after AppliedSeq() is announced or later ones are queued,
  // but it has not arrived. How long to wait before rebuilding is the caller's call.
  bool HasGap() const;

  // A build starts (any mode -> Building). Queued and future changes are kept.
  void StartBuild();
  // The build was installed at epoch b. Returns the changes to apply as ONE
  // combined catch-up transaction: queued changes with epoch > b, in
  // sequence order, through the first gap. Changes with epoch <= b are
  // dropped (they are in the build). After the caller applies them and calls
  // FinishCatchUp, the mode is Synced at the last one (or at the highest
  // sequence number <= b seen, if none). If a gap remains, later changes stay
  // queued and HasGap() reports it.
  std::vector<CommittedChange> TakeCatchUp(Epoch b);
  // The catch-up changes were applied: Synced at the point TakeCatchUp
  // computed (its last change, or the build's boundary if it returned none).
  void FinishCatchUp();
  // Puts the changes TakeCatchUp returned back in the queue: FinishCatchUp
  // then syncs at the build's boundary and they are taken as ready changes.
  void ReturnCatchUp(std::vector<CommittedChange> changes);
  // The build failed or was abandoned: back to Stale, queue kept.
  void AbandonBuild();
  // The state became invalid (maintenance failed): Synced -> Stale.
  void Invalidate();

  std::size_t QueuedCount() const { return queue.size(); }
  std::size_t QueuedBytes() const;

private:
  Mode mode = Mode::Stale;
  Seq applied_seq = 0;
  Epoch applied_epoch = 0;
  Seq highest_seen = 0;
  Seq catch_up_seq = 0;
  Epoch catch_up_epoch = 0;
  Seq boundary_seq = 0;
  Epoch boundary_epoch = 0;
  std::map<Seq, CommittedChange> queue;
};

} // namespace accelerator_codegen::aux_state::runtime
