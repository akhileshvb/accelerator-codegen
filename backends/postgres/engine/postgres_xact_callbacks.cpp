// GENERATED BACKEND - PostgreSQL transaction lifecycle of captured writes
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/engine/postgres_xact_callbacks.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <string>
#include <vector>

#include "backends/postgres/engine/accelerator_tables.hpp"
#include "backends/postgres/state/messages.hpp"
#include "backends/postgres/state/shard_worker.hpp"
#include "backends/postgres/state/shared.hpp"

extern "C" {
#include <postgres.h>
#include <access/xact.h>
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// What one (sub)transaction level staged.
struct Frame {
  int level = 0;
  std::vector<runtime::ColumnarBatchRef> batches;
  std::set<core::TableId> written;
  std::set<core::TableId> invalidated;
};

struct Xact {
  std::vector<Frame> frames; // ascending level
  // PRE_COMMIT: the commits to ship, with their epochs once taken.
  std::vector<CommitMessage> commits;
  bool epochs_taken = false;
};

Xact g_xact;
bool g_registered = false;

Frame &FrameAt(int level) {
  auto &frames = g_xact.frames;
  if (frames.empty() || frames.back().level < level) {
    frames.push_back(Frame{level, {}, {}, {}});
  }
  return frames.back();
}

void Merge(Frame &into, Frame &&from) {
  into.batches.insert(into.batches.end(), std::make_move_iterator(from.batches.begin()),
                      std::make_move_iterator(from.batches.end()));
  into.written.merge(from.written);
  into.invalidated.merge(from.invalidated);
}

// The ending subtransaction is still current at the callback.
void EndSubtransaction(bool committed) {
  auto &frames = g_xact.frames;
  const int level = GetCurrentTransactionNestLevel();
  if (frames.empty() || frames.back().level != level) {
    return;
  }
  Frame ended = std::move(frames.back());
  frames.pop_back();
  if (!committed) {
    return;
  }
  if (!frames.empty() && frames.back().level == level - 1) {
    Merge(frames.back(), std::move(ended));
  } else {
    ended.level = level - 1;
    frames.push_back(std::move(ended));
  }
}

// The commits as the accelerators that depend on what the transaction wrote
// will see them.
void PlanCommits(const Frame &frame, const std::vector<int> &accelerators) {
  g_xact.commits.clear();
  for (const int index : accelerators) {
    CommitMessage commit;
    commit.accelerator = static_cast<uint32_t>(index);
    for (const auto &batch : frame.batches) {
      if (Depends(index, batch->table)) {
        commit.batches.push_back(batch);
      }
    }
    commit.invalidates = std::any_of(frame.invalidated.begin(), frame.invalidated.end(),
                                     [&](core::TableId table) { return Depends(index, table); });
    g_xact.commits.push_back(std::move(commit));
  }
}

void PreCommit() {
  auto &frames = g_xact.frames;
  while (frames.size() > 1) { // subtransactions that ended before the commit
    Frame last = std::move(frames.back());
    frames.pop_back();
    Merge(frames.back(), std::move(last));
  }
  if (frames.empty()) {
    return;
  }
  const auto accelerators = AcceleratorsOn(frames.front().written);
  if (accelerators.empty()) {
    return;
  }
  std::string error;
  try {
    PlanCommits(frames.front(), accelerators);
  } catch (const std::exception &ex) {
    error = ex.what(); // raise outside the handler: ereport does not return
  }
  if (!error.empty()) {
    ereport(ERROR, (errmsg("accelerator commit preparation failed: %s", error.c_str())));
  }
  // May wait for a gate (and be cancelled: the abort callback releases).
  const auto epochs = shared::TakeCommitEpochs(accelerators, GetTopTransactionId());
  for (std::size_t i = 0; i < epochs.size(); i++) {
    g_xact.commits[i].epoch = epochs[i];
  }
  g_xact.epochs_taken = true;
}

// Past the point of no return: nothing here may raise.
void Ship() {
  const auto deadline = std::chrono::steady_clock::now() + shared::CommitShipTimeout();
  for (const auto &commit : g_xact.commits) {
    if (shared::FaultDropsCommit()) {
      continue;
    }
    try {
      std::string bytes;
      EncodeCommit(commit, bytes);
      if (!shard::SendCommit(static_cast<int>(commit.accelerator), bytes, deadline)) {
        elog(WARNING, "accelerator %u: could not queue commit %llu; it will rebuild", commit.accelerator,
             static_cast<unsigned long long>(commit.epoch));
      }
    } catch (const std::exception &ex) {
      elog(WARNING, "accelerator %u: could not ship commit %llu: %s", commit.accelerator,
           static_cast<unsigned long long>(commit.epoch), ex.what());
    }
  }
}

void XactCallback(XactEvent event, void *) {
  switch (event) {
  case XACT_EVENT_PRE_PREPARE:
    if (!g_xact.frames.empty() && !g_xact.frames.front().written.empty()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("cannot PREPARE a transaction that modified accelerator-tracked tables")));
    }
    break;
  case XACT_EVENT_PRE_COMMIT:
    PreCommit();
    break;
  case XACT_EVENT_COMMIT:
    shared::ReleaseGates(); // the commit is visible now
    Ship();
    g_xact = Xact{};
    break;
  case XACT_EVENT_ABORT:
    shared::ReleaseGates();
    if (g_xact.epochs_taken) {
      for (auto &commit : g_xact.commits) {
        commit.batches.clear();
        commit.invalidates = false;
        commit.empty = true;
      }
      Ship();
    }
    g_xact = Xact{};
    break;
  default:
    break;
  }
}

void SubXactCallback(SubXactEvent event, SubTransactionId, SubTransactionId, void *) {
  if (event == SUBXACT_EVENT_COMMIT_SUB || event == SUBXACT_EVENT_ABORT_SUB) {
    EndSubtransaction(event == SUBXACT_EVENT_COMMIT_SUB);
  }
}

} // namespace

void RegisterPostgresXactCallbacks() {
  if (!g_registered) {
    RegisterXactCallback(XactCallback, nullptr);
    RegisterSubXactCallback(SubXactCallback, nullptr);
    g_registered = true;
  }
}

void NoteWrite(core::TableId table) { FrameAt(GetCurrentTransactionNestLevel()).written.insert(table); }

void StageBatch(runtime::ColumnarBatchRef batch) {
  NoteWrite(batch->table);
  FrameAt(GetCurrentTransactionNestLevel()).batches.push_back(std::move(batch));
}

void StageInvalidation(core::TableId table) {
  NoteWrite(table);
  FrameAt(GetCurrentTransactionNestLevel()).invalidated.insert(table);
}

namespace shared {

bool TransactionWrote(const std::vector<core::TableId> &tables) {
  return std::any_of(g_xact.frames.begin(), g_xact.frames.end(), [&](const Frame &frame) {
    return std::any_of(tables.begin(), tables.end(),
                       [&](core::TableId table) { return frame.written.count(table) != 0; });
  });
}

} // namespace shared

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
