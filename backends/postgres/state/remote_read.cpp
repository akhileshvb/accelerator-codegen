// GENERATED BACKEND
//
// The reader side of a shard worker read (shard_worker.hpp): send the request,
// wait for the reply, write the rows into the function's materialized result.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/state/shard_worker.hpp"

#include <algorithm>

#include "backends/postgres/api/pg_sink.hpp"
#include "backends/postgres/engine/accelerator_tables.hpp"
#include "backends/postgres/state/messages.hpp"
#include "backends/postgres/state/shard_gucs.hpp"
#include "backends/postgres/state/shared.hpp"

extern "C" {
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <utils/guc.h>
#include <utils/snapmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

using std::chrono::milliseconds;

// How often a waiting backend checks for cancel and terminate requests.
constexpr auto kInterruptEvery = milliseconds(20);

// The slot this backend holds, so that a backend that exits while waiting
// (terminated) gives it up.
struct Held {
  ShardSegment *segment = nullptr;
  uint32_t slot = 0;
  bool exit_hook = false;
} g_held;

void ReleaseHeld() {
  if (g_held.segment) {
    g_held.segment->ReleaseSlot(g_held.slot);
    g_held.segment = nullptr;
  }
}

void ReleaseHeldOnExit(int, Datum) { ReleaseHeld(); }

void Hold(ShardSegment &segment, uint32_t slot) {
  if (!g_held.exit_hook) {
    before_shmem_exit(ReleaseHeldOnExit, 0);
    g_held.exit_hook = true;
  }
  g_held.segment = &segment;
  g_held.slot = slot;
}

// Waits for the reply, staying interruptible: an interrupt that raises an
// error gives up the slot first. False: no reply (closed, or too late).
bool AwaitReply(ShardSegment &segment, uint32_t slot, Clock::time_point deadline, std::string &reply) {
  bool received = false;
  PG_TRY();
  {
    for (;;) {
      if (segment.WaitResponse(slot, std::min(deadline, Clock::now() + kInterruptEvery))) {
        received = segment.ReceiveResponse(slot, reply, deadline);
        break;
      }
      if (Clock::now() >= deadline) {
        break;
      }
      CHECK_FOR_INTERRUPTS();
    }
  }
  PG_CATCH();
  {
    ReleaseHeld();
    PG_RE_THROW();
  }
  PG_END_TRY();
  return received;
}

} // namespace

Remote RemoteRead(int index, uint32_t member, runtime::Epoch epoch, const core::QueryArguments &arguments,
                  FunctionCallInfo fcinfo) {
  ShardSegment *segment = Segment(index);
  if (!segment) {
    return Remote::Fallback;
  }
  EnsureWorker(index);
  // The state was built under the worker's search_path: another one may name
  // other tables.
  if (segment->state.load() != ShardSegment::kRunning ||
      segment->GetText(ShardSegment::kSearchPath) != GetConfigOption("search_path", false, false)) {
    return Remote::Fallback;
  }
  const int claimed = segment->ClaimSlot();
  if (claimed < 0) {
    return Remote::Fallback;
  }
  const auto slot = static_cast<uint32_t>(claimed);
  Hold(*segment, slot);

  const auto deadline = Clock::now() + milliseconds(ReadTimeoutMs());
  ReadRequest request;
  request.slot = slot;
  request.member = member;
  request.deadline_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
  request.epoch = epoch;
  request.max_wait_us = static_cast<uint64_t>(ReadWaitMs()) * 1000;
  request.arguments = arguments;
  std::string message;
  EncodeRequest(request, message);

  std::string reply;
  const bool received =
      segment->SendRequest(slot, message, deadline) && AwaitReply(*segment, slot, deadline, reply);
  ReleaseHeld(); // before anything below can raise
  if (!received) {
    return Remote::Fallback;
  }
  ReadResponse response;
  try {
    response = DecodeResponse(reply);
  } catch (const std::exception &) {
    return Remote::Fallback;
  }
  if (!response.served) {
    return Remote::Fallback;
  }
  auto *return_set_info = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
  detail::PgSink sink(return_set_info->setResult, return_set_info->setDesc);
  for (const auto &row : response.rows) {
    sink.WriteRow(row);
  }
  return Remote::Served;
}

bool ShardPosition(const PostgresAccelerator &accelerator, int &index, uint32_t &member) {
  index = shared::Activate(accelerator.Shard());
  if (index < 0) {
    return false;
  }
  const auto members = ShardMembers(accelerator.Shard());
  const auto it = std::find_if(members.begin(), members.end(),
                               [&](const PostgresAccelerator *m) { return m->Id() == accelerator.Id(); });
  member = static_cast<uint32_t>(it - members.begin());
  return it != members.end();
}

bool ServeFromShard(const PostgresAccelerator &accelerator, const core::QueryArguments &arguments,
                    FunctionCallInfo fcinfo) {
  int index;
  uint32_t member;
  if (!ShardPosition(accelerator, index, member) || ForceFallback()) {
    return false;
  }
  // A transaction's own writes to the tables are in no worker's state.
  if (shared::TransactionWrote(HostTables(index))) {
    return false;
  }
  const auto epoch = shared::ReaderEpoch(index, GetActiveSnapshot());
  return epoch && RemoteRead(index, member, *epoch, arguments, fcinfo) == Remote::Served;
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
