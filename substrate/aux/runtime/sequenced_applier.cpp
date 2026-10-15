// SCAFFOLDING
// C3: SequencedApplier, ordering and catch-up bookkeeping (no locks).
#include "substrate/aux/runtime/sequenced_applier.hpp"

#include <algorithm>

namespace accelerator_codegen::aux_state::runtime {

bool SequencedApplier::Offer(CommittedChange change) {
  const Seq seq = change.seq;
  highest_seen = std::max(highest_seen, seq);
  if (seq <= applied_seq) {
    return false;
  }
  return queue.emplace(seq, std::move(change)).second;
}

void SequencedApplier::Announce(Seq seq) { highest_seen = std::max(highest_seen, seq); }

std::vector<CommittedChange> SequencedApplier::TakeReady() {
  std::vector<CommittedChange> out;
  if (mode != Mode::Synced) {
    return out;
  }
  Seq next = applied_seq + 1;
  for (auto it = queue.begin(); it != queue.end() && it->first == next;) {
    out.push_back(std::move(it->second));
    it = queue.erase(it);
    ++next;
  }
  return out;
}

void SequencedApplier::MarkApplied(const CommittedChange &change) {
  applied_seq = change.seq;
  // Empty commits may carry epoch 0.
  applied_epoch = std::max(applied_epoch, change.epoch);
}

bool SequencedApplier::HasGap() const {
  if (mode == Mode::Building) {
    return false;
  }
  return highest_seen > applied_seq && queue.find(applied_seq + 1) == queue.end();
}

void SequencedApplier::StartBuild() {
  mode = Mode::Building;
  build_epoch.reset();
}

void SequencedApplier::SetBuildEpoch(Epoch b) { build_epoch = b; }

// The build contains exactly the commits with epoch <= b. Queued non-empty
// changes with epoch <= b are therefore dropped, and s_b (the highest
// sequence number covered by the build) is the larger of applied_seq and the
// highest dropped seq. Empty commits carry no (reliable) epoch: one is dropped
// when it lies below the first kept change (so it precedes it in sequence
// order, hence in the build or irrelevant to it); empty commits after the
// first kept change, or when nothing is kept, stay queued and are returned by
// the consecutive run (the caller just MarkApplied's them).
//
// Engines where epoch == seq (Postgres, MySQL): s_b is exactly b. Sparse
// epochs (DuckDB: commit id + 1): every commit is offered to the applier, so
// the max-seq rule is exact. If the next queued seq is not s_b + 1 a gap
// remains (nothing is returned; later changes stay queued, HasGap() reports it).
std::vector<CommittedChange> SequencedApplier::TakeCatchUp(Epoch b) {
  Seq s_b = applied_seq;
  Seq first_kept = 0; // smallest seq of a non-empty change with epoch > b
  for (const auto &[seq, change] : queue) {
    if (!change.empty_commit && change.epoch > b) {
      first_kept = seq;
      break;
    }
  }
  for (auto it = queue.begin(); it != queue.end();) {
    const CommittedChange &c = it->second;
    const bool drop =
        c.empty_commit ? (first_kept != 0 && it->first < first_kept) : c.epoch <= b;
    if (drop) {
      s_b = std::max(s_b, it->first);
      it = queue.erase(it);
    } else {
      ++it;
    }
  }
  // Anything left at or below s_b is covered as well.
  while (!queue.empty() && queue.begin()->first <= s_b) {
    queue.erase(queue.begin());
  }
  std::vector<CommittedChange> out;
  Seq next = s_b + 1;
  for (auto it = queue.begin(); it != queue.end() && it->first == next;) {
    out.push_back(std::move(it->second));
    it = queue.erase(it);
    ++next;
  }
  catch_up_seq = out.empty() ? s_b : out.back().seq;
  catch_up_epoch = std::max({applied_epoch, b, out.empty() ? Epoch{0} : out.back().epoch});
  return out;
}

void SequencedApplier::FinishCatchUp() {
  mode = Mode::Synced;
  applied_seq = catch_up_seq;
  applied_epoch = catch_up_epoch;
  build_epoch.reset();
}

void SequencedApplier::AbandonBuild() {
  mode = Mode::Stale;
  build_epoch.reset();
}

void SequencedApplier::Invalidate() {
  if (mode == Mode::Synced) {
    mode = Mode::Stale;
  }
}

std::size_t SequencedApplier::QueuedBytes() const {
  std::size_t total = 0;
  for (const auto &[seq, change] : queue) {
    for (const auto &batch : change.batches) {
      if (batch) {
        total += ByteSize(*batch);
      }
    }
  }
  return total;
}

} // namespace accelerator_codegen::aux_state::runtime
