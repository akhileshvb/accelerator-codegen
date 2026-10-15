// TPC-H Q15, base parallel PostgreSQL kernel.  The leader scans the small
// supplier table; participants aggregate revenue per l_suppkey over their share
// of the lineitem parallel scan and the leader adds the sparse partial results.
#include "tpch/q15/postgres/base/q15_common.hpp"

namespace accelerator_codegen::pg_base::q15 {
namespace {

constexpr const char *kTask = "tpch_q15_revenue";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	Revenue rev(in.Get<uint64_t>());
	ctx.Scan(0, [&](const Batch &b) { rev.Consume(b); });
	ctx.SetOutput(rev.Serialize());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	uint64_t dense_size = 0;
	std::vector<Supplier> suppliers = ScanSuppliers(dense_size);
	ByteWriter in;
	in.Put<uint64_t>(dense_size);
	Revenue rev(dense_size);
	for (const auto &part : RunParallel(kTask, in.buf, {LineitemSpec(p)}, ParallelParticipants())) {
		ByteReader r(part);
		for (const auto &e : r.GetVector<Ent>()) {
			rev.Add(e.key, e.v);
		}
	}
	Emit(rev, suppliers, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({15, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q15
