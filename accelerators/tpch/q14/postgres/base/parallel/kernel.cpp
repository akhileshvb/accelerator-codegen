// TPC-H Q14, base parallel PostgreSQL kernel.  The leader scans the (small)
// part table, builds the promo classification and broadcasts it; every
// participant probes its share of the lineitem parallel scan.
#include "tpch/q14/postgres/base/q14_common.hpp"

namespace accelerator_codegen::pg_base::q14 {
namespace {

constexpr const char *kTask = "tpch_q14_probe";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const PartClass pc = Get(in);
	Agg agg;
	ctx.Scan(0, [&](const Batch &b) { Accumulate(b, pc, agg); });
	ByteWriter w;
	w.Put(agg.total);
	w.Put(agg.promo);
	w.Put<uint8_t>(agg.any);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const PartClass pc = BuildPartClass();
	ByteWriter in;
	Put(in, pc);
	Agg agg;
	for (const auto &part : RunParallel(kTask, in.buf, {LineitemSpec(p)}, ParallelParticipants())) {
		ByteReader r(part);
		agg.total += r.Get<__int128>();
		agg.promo += r.Get<__int128>();
		agg.any |= r.Get<uint8_t>() != 0;
	}
	Emit(agg, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({14, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q14
