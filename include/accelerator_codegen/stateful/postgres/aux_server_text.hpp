// Maintenance diagnostics of the shared state worker, without PostgreSQL
// headers (see aux_server.hpp).  When accel.shared_state is on, maintenance
// runs in the worker, so the backend-local decision log and the forced-action
// switch are bridged through shared memory.
#pragma once

#include <string>

namespace accelerator_codegen::aux_state::engine::postgres::aux_server {

// Forces the worker's next maintenance decisions ("" clears).  No-op unless
// accel.shared_state is on.
void SetForcedMaintenance(const std::string &action_name);
// The worker's last maintenance action name ("" if shared state is off or it
// recorded none) and its decision text (as core::FormatLastMaintenanceDecision).
std::string LastMaintenanceActionName();
std::string LastMaintenanceDecisionText();

} // namespace accelerator_codegen::aux_state::engine::postgres::aux_server
