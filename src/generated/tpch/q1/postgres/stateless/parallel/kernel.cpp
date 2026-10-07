// TPC-H Q1, stateless parallel PostgreSQL kernel: each participant aggregates
// its share of the lineitem parallel scan; the leader merges the group maps.
#include "tpch/q1/postgres/stateless/q1_common.hpp"

namespace accelerator_codegen::pg_stateless::q1 {
namespace {

constexpr const char *kTask = "tpch_q1_scan";

void Task(TaskContext &ctx) {
	Aggregator agg;
	ctx.Scan(0, [&](const Batch &b) { agg.Consume(b); });
	ctx.SetOutput(Serialize(agg.groups));
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	GroupMap merged;
	for (const auto &part : RunParallel(kTask, std::string_view(), {Spec(p)}, ParallelParticipants())) {
		Deserialize(part, merged);
	}
	Emit(merged, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({1, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q1
