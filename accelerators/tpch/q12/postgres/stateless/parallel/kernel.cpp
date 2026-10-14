// TPC-H Q12, stateless parallel PostgreSQL kernel.
//   round 1: participants scan their share of lineitem (filters pushed into the
//            scan, date comparisons in SIMD) and return the surviving
//            (orderkey, shipmode) pairs; the leader concatenates them.
//   round 2: the pairs are broadcast, every participant indexes them and probes
//            its share of orders, returning 4 counters; the leader adds them.
#include "tpch/q12/postgres/stateless/q12_common.hpp"

namespace accelerator_codegen::pg_stateless::q12 {
namespace {

constexpr const char *kLineitemTask = "tpch_q12_lineitem";
constexpr const char *kOrdersTask = "tpch_q12_orders";

void PutParams(ByteWriter &w, const Params &p) {
	w.PutString(p.mode[0]);
	w.PutString(p.mode[1]);
	w.Put(p.start);
	w.Put(p.end_excl);
	w.Put(p.modes);
}

Params GetParams(ByteReader &r) {
	Params p;
	p.mode[0] = r.GetString();
	p.mode[1] = r.GetString();
	p.start = r.Get<int32_t>();
	p.end_excl = r.Get<int32_t>();
	p.modes = r.Get<int>();
	return p;
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	Hits hits;
	ctx.Scan(0, [&](const Batch &b) { CollectLineitem(b, p, hits); });
	ByteWriter w;
	w.PutVector(hits.keys);
	w.PutVector(hits.mode);
	ctx.SetOutput(w.Take());
}

void OrdersTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	Hits hits;
	hits.keys = in.GetVector<int64_t>();
	hits.mode = in.GetVector<uint8_t>();
	OrderProbe probe;
	probe.Build(hits);
	Totals t = {};
	ctx.Scan(0, [&](const Batch &b) { ProbeOrders(b, probe, t); });
	ByteWriter w;
	w.Put(t);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int participants = ParallelParticipants();
	ByteWriter in1;
	PutParams(in1, p);
	Hits all;
	for (const auto &part : RunParallel(kLineitemTask, in1.buf, {LineitemSpec(p)}, participants)) {
		ByteReader r(part);
		const auto k = r.GetVector<int64_t>();
		const auto m = r.GetVector<uint8_t>();
		all.keys.insert(all.keys.end(), k.begin(), k.end());
		all.mode.insert(all.mode.end(), m.begin(), m.end());
	}
	Totals t = {};
	if (!all.keys.empty()) {
		ByteWriter in2;
		in2.PutVector(all.keys);
		in2.PutVector(all.mode);
		for (const auto &part : RunParallel(kOrdersTask, in2.buf, {OrdersSpec()}, participants)) {
			ByteReader r(part);
			const Totals x = r.Get<Totals>();
			for (int m = 0; m < 2; m++) {
				for (int h = 0; h < 2; h++) {
					t[m][h] += x[m][h];
				}
			}
		}
	}
	Emit(p, t, out);
}

const TaskRegistrar kLineitemRegistrar(kLineitemTask, &LineitemTask);
const TaskRegistrar kOrdersRegistrar(kOrdersTask, &OrdersTask);
const KernelRegistrar kRegistrar({12, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q12
