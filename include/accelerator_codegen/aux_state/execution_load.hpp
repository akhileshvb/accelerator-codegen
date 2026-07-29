#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace accelerator_codegen::aux_state {

enum class ExecutionKind : uint8_t {
	AuxQuery = 0,
	FallbackQuery = 1,
	MaintenanceApply = 2,
	LazyRebuild = 3,
};

struct ExecutionLoadSnapshot {
	uint32_t active_aux_queries = 0;
	uint32_t active_fallback_queries = 0;
	uint32_t active_rebuilds = 0;
	uint32_t active_maintenance = 0;
	uint32_t total_active = 0;
	double total_weight_us = 0.0;
};

struct ExecutionToken {
	uint64_t id = 0;
};

class ExecutionLoadRegistry {
public:
	ExecutionToken Enter(ExecutionKind kind, const std::string &accelerator_id, double weight_us);
	void Exit(const ExecutionToken &token);
	ExecutionLoadSnapshot Snapshot() const;

private:
	struct ActiveEntry {
		ExecutionKind kind = ExecutionKind::AuxQuery;
		std::string accelerator_id;
		double weight_us = 0.0;
	};

	mutable std::mutex mutex;
	std::unordered_map<uint64_t, ActiveEntry> active;
	std::atomic<uint64_t> next_token_id {1};
};

ExecutionLoadRegistry &GetExecutionLoadRegistry();
ExecutionLoadRegistry &ExecutionLoadRegistryForTests();

class ActiveExecutionScope {
public:
	ActiveExecutionScope(ExecutionKind kind, const std::string &accelerator_id, double weight_us = 0.0);
	~ActiveExecutionScope();
	ActiveExecutionScope(const ActiveExecutionScope &) = delete;
	ActiveExecutionScope &operator=(const ActiveExecutionScope &) = delete;
	ActiveExecutionScope(ActiveExecutionScope &&other) noexcept;
	ActiveExecutionScope &operator=(ActiveExecutionScope &&other) noexcept;

private:
	bool active = false;
	ExecutionToken token;
};

} // namespace accelerator_codegen::aux_state
