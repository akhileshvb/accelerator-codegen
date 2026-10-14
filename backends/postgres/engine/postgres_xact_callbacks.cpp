// GENERATED BACKEND - PostgreSQL transaction callbacks + mutation fan-out
#include "backends/postgres/engine/postgres_xact_callbacks.hpp"
#include "backends/postgres/state/aux_server.hpp"
#include "backends/postgres/state/aux_sync.hpp"
#include "backends/postgres/state/postgres_aux_backend.hpp"
#include "backends/postgres/engine/snapshot_token.hpp"

#include <exception>
#include <mutex>
#include <vector>

extern "C" {
#include <postgres.h>
#include <access/xact.h>
#include <utils/snapmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

static bool g_callbacks_registered = false;
static std::mutex g_backends_mu;
static std::vector<PostgresAuxBackend *> g_mutation_backends;

void XactCallbackBridge(XactEvent event, void *arg) {
  (void)arg;
  if (aux_server::InServer()) {
    return; // the state server's own transactions are not accelerator writes
  }
  const auto backends = RegisteredPostgresMutationBackends();
  switch (event) {
  case XACT_EVENT_PRE_PREPARE:
    aux_sync::CheckPrepareAllowed();
    break;
  case XACT_EVENT_PRE_COMMIT: {
    aux_sync::NoteTopXid();
    // Shared state: decide what ships and take the commit gate (may wait).
    aux_server::PrepareCommit();
    // Statement portals have already popped their snapshot; SPI scans in
    // StageMaintenance need an active one.  Use the latest snapshot (not the
    // transaction snapshot, which is stale under REPEATABLE READ): the
    // participants' counter check below is taken after it, so every
    // transaction it sees is known to the synchronised store.
    PushActiveSnapshot(GetLatestSnapshot());
    PG_TRY();
    {
      try {
        for (auto *participant : aux_sync::Participants()) {
          participant->BeforePreCommit();
        }
        for (auto *backend : backends) {
          if (backend) {
            backend->OnPreCommit();
          }
        }
      } catch (const std::exception &ex) {
        ereport(ERROR,
                (errmsg("accelerator pre-commit failed: %s", ex.what())));
      }
    }
    PG_CATCH();
    {
      PopActiveSnapshot();
      PG_RE_THROW();
    }
    PG_END_TRY();
    PopActiveSnapshot();
    break;
  }
  case XACT_EVENT_COMMIT: {
    // Past the point of no return: nothing here may raise an error.
    const aux_sync::CommitInfo &published = aux_sync::PublishCommit();
    aux_sync::CommitInfo info = published;
    // Shared state: hand the (now visible and counted) commit to the worker.
    aux_server::ShipCommit(published);
    aux_server::ResetStage();
    for (auto *backend : backends) {
      if (!backend) {
        continue;
      }
      try {
        backend->OnCommit();
      } catch (...) {
        info.maintenance_failed = true;
      }
    }
    for (auto *participant : aux_sync::Participants()) {
      try {
        participant->AfterCommit(info);
      } catch (...) {
      }
    }
    aux_sync::ResetTransaction();
    // Drop statement pins so a later xact does not inherit frozen epochs
    // keyed by a reused snapshot encoding from this xact.
    ClearSnapshotPins();
    break;
  }
  case XACT_EVENT_ABORT:
    aux_server::ResetStage();
    for (auto *backend : backends) {
      if (!backend) {
        continue;
      }
      try {
        backend->OnAbort();
      } catch (...) {
      }
    }
    aux_sync::ReleaseAbort();
    ClearSnapshotPins();
    break;
  default:
    break;
  }
}

void SubXactCallbackBridge(SubXactEvent event, SubTransactionId my_subid,
                           SubTransactionId parent_subid, void *arg) {
  (void)my_subid;
  (void)parent_subid;
  (void)arg;
  if (aux_server::InServer()) {
    return;
  }
  if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB) {
    return;
  }
  // Called while the ending subtransaction is still current.
  const int level = GetCurrentTransactionNestLevel();
  if (level < 2) {
    return;
  }
  const auto depth = static_cast<std::size_t>(level - 1);
  if (event == SUBXACT_EVENT_COMMIT_SUB) {
    aux_server::OnSubCommit(depth);
  } else {
    aux_server::OnSubAbort(depth);
  }
  const auto backends = RegisteredPostgresMutationBackends();
  for (auto *backend : backends) {
    if (!backend) {
      continue;
    }
    if (event == SUBXACT_EVENT_COMMIT_SUB) {
      try {
        backend->OnSubCommit(depth);
      } catch (const std::exception &ex) {
        ereport(ERROR, (errmsg("accelerator subtransaction commit failed: %s",
                               ex.what())));
      }
    } else {
      try {
        backend->OnSubAbort(depth);
      } catch (...) {
      }
    }
  }
}

} // anonymous namespace

void RegisterPostgresXactCallbacks() {
  if (g_callbacks_registered)
    return;
  RegisterXactCallback(XactCallbackBridge, nullptr);
  RegisterSubXactCallback(SubXactCallbackBridge, nullptr);
  // TokenForSnapshot: pin at ExecutorStart under the statement snapshot.
  RegisterSnapshotPinExecutorHook();
  g_callbacks_registered = true;
}

bool RegisterPostgresBackendForMutations() {
  bool first = !g_callbacks_registered;
  if (first) {
    RegisterPostgresXactCallbacks();
  }
  return first;
}

void RegisterPostgresMutationBackend(PostgresAuxBackend *backend) {
  if (!backend)
    return;
  std::lock_guard<std::mutex> lock(g_backends_mu);
  for (auto *b : g_mutation_backends) {
    if (b == backend)
      return;
  }
  g_mutation_backends.push_back(backend);
}

std::vector<PostgresAuxBackend *> RegisteredPostgresMutationBackends() {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  return g_mutation_backends;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
