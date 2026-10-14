// Live PostgreSQL host bridge. Observer + maintenance-policy helpers used by
// TPC-H SRFs, transition-table capture, and differential/calibration harnesses.
// Compiled into MODULE accelerator_codegen_pg (not the host shared lib).

#include <mutex>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "backends/postgres/state/aux_server_text.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"

using namespace accelerator_codegen::aux_state;

namespace {

struct BridgeState {
  struct Observer {
    std::vector<core::MutationBatch> captured_mutations;
    int pending_mutations = 0;
  };
  Observer observer;
};

static std::mutex g_mutex;
static BridgeState *g_bridge = nullptr;

BridgeState &State() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_bridge)
    g_bridge = new BridgeState();
  return *g_bridge;
}

} // anonymous namespace

extern "C" {

void accel_pg_init_backend(const char * /*backing_dir*/) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_bridge) {
    g_bridge = new BridgeState();
  }
}

void accel_pg_observer_note_mutation(void) {
  auto &s = State();
  s.observer.pending_mutations++;
  s.observer.captured_mutations.push_back(core::MutationBatch{});
}

int accel_pg_get_observer_mutation_count() {
  return static_cast<int>(State().observer.captured_mutations.size());
}

void accel_pg_set_force_maintenance_action(const char *action_name) {
  core::ClearMaintenanceDecisionLog();
  core::SetForcedMaintenanceActionName(action_name ? action_name : "");
  accelerator_codegen::aux_state::engine::postgres::aux_server::
      SetForcedMaintenance(action_name ? action_name : "");
}

const char *accel_pg_get_last_maintenance_action(void) {
  static thread_local std::string cached;
  cached = accelerator_codegen::aux_state::engine::postgres::aux_server::
      LastMaintenanceActionName();
  if (!cached.empty()) {
    return cached.c_str(); // shared state: maintenance ran in the worker
  }
  const auto entry = core::LastMaintenanceDecision();
  if (!entry || !entry->valid) {
    cached = "None";
  } else {
    cached = maintenance::MaintenanceActionName(entry->staged_action);
  }
  return cached.c_str();
}

void accel_pg_reset_observer() {
  auto &s = State();
  s.observer = BridgeState::Observer{};
}

} // extern "C"
