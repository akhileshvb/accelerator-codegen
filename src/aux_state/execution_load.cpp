#include "accelerator_codegen/aux_state/execution_load.hpp"

namespace accelerator_codegen::aux_state {
namespace {

ExecutionLoadRegistry g_execution_load_registry;

} // namespace

ExecutionToken ExecutionLoadRegistry::Enter(ExecutionKind kind, const std::string &accelerator_id, double weight_us) {
	ExecutionToken token;
	token.id = next_token_id.fetch_add(1);
	std::lock_guard<std::mutex> guard(mutex);
	active[token.id] = ActiveEntry {kind, accelerator_id, weight_us};
	return token;
}

void ExecutionLoadRegistry::Exit(const ExecutionToken &token) {
	if (token.id == 0) {
		return;
	}
	std::lock_guard<std::mutex> guard(mutex);
	active.erase(token.id);
}

ExecutionLoadSnapshot ExecutionLoadRegistry::Snapshot() const {
	std::lock_guard<std::mutex> guard(mutex);
	ExecutionLoadSnapshot snapshot;
	for (const auto &entry : active) {
		switch (entry.second.kind) {
		case ExecutionKind::AuxQuery:
			snapshot.active_aux_queries++;
			break;
		case ExecutionKind::FallbackQuery:
			snapshot.active_fallback_queries++;
			break;
		case ExecutionKind::LazyRebuild:
			snapshot.active_rebuilds++;
			break;
		case ExecutionKind::MaintenanceApply:
			snapshot.active_maintenance++;
			break;
		}
		snapshot.total_weight_us += entry.second.weight_us;
	}
	snapshot.total_active = static_cast<uint32_t>(active.size());
	return snapshot;
}

ExecutionLoadRegistry &GetExecutionLoadRegistry() {
	return g_execution_load_registry;
}

ExecutionLoadRegistry &ExecutionLoadRegistryForTests() {
	return g_execution_load_registry;
}

ActiveExecutionScope::ActiveExecutionScope(ExecutionKind kind, const std::string &accelerator_id, double weight_us) {
	token = GetExecutionLoadRegistry().Enter(kind, accelerator_id, weight_us);
	active = token.id != 0;
}

ActiveExecutionScope::~ActiveExecutionScope() {
	if (active) {
		GetExecutionLoadRegistry().Exit(token);
	}
}

ActiveExecutionScope::ActiveExecutionScope(ActiveExecutionScope &&other) noexcept
    : active(other.active), token(other.token) {
	other.active = false;
	other.token = {};
}

ActiveExecutionScope &ActiveExecutionScope::operator=(ActiveExecutionScope &&other) noexcept {
	if (this != &other) {
		if (active) {
			GetExecutionLoadRegistry().Exit(token);
		}
		active = other.active;
		token = other.token;
		other.active = false;
		other.token = {};
	}
	return *this;
}

} // namespace accelerator_codegen::aux_state
