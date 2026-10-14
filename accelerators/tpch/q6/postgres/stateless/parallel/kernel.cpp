// TPC-H Q6, stateless parallel PostgreSQL kernel: every participant sums its
// share of the lineitem parallel scan; the leader adds the partial sums.
#include "tpch/q6/postgres/stateless/q6_common.hpp"

namespace accelerator_codegen::pg_stateless::q6 {
namespace {

constexpr const char *kTask = "tpch_q6_scan";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = in.Get<Params>();
	(void)p; // filters were applied by the scan spec built from p
	Agg agg;
	ctx.Scan(0, [&](const Batch &b) { Accumulate(b, agg); });
	ByteWriter w;
	w.Put(agg.sum);
	w.Put<uint8_t>(agg.any);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	ByteWriter in;
	in.Put(p);
	Agg agg;
	for (const auto &part : RunParallel(kTask, in.buf, {Spec(p)}, ParallelParticipants())) {
		ByteReader r(part);
		agg.sum += r.Get<__int128>();
		agg.any |= r.Get<uint8_t>() != 0;
	}
	Emit(agg, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({6, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q6
