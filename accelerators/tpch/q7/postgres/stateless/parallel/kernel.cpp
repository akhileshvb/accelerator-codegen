// TPC-H Q7, stateless parallel PostgreSQL kernel.
// Round 0 (leader): nation / supplier / customer -> dense mask tables.
// Round 1: parallel orders scan -> (orderkey, customer mask) pairs, merged by
//          the leader into the dense order table.
// Round 2: parallel lineitem scan (shipdate pushed down) probing the tables;
//          each participant returns its 2x2 revenue sums.
#include "tpch/q7/postgres/stateless/q7_common.hpp"

namespace accelerator_codegen::pg_stateless::q7 {
namespace {

constexpr const char *kOrdersTask = "tpch_q7_orders";
constexpr const char *kLineitemTask = "tpch_q7_lineitem";

void OrdersTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable cust;
	cust.Read(in);
	std::vector<int64_t> keys;
	std::vector<uint8_t> masks;
	ctx.Scan(0, [&](const Batch &b) {
		OrdersBatch(b, cust, [&](int64_t k, uint8_t m) {
			keys.push_back(k);
			masks.push_back(m);
		});
	});
	ByteWriter w;
	w.PutVector(keys);
	w.PutVector(masks);
	ctx.SetOutput(w.Take());
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable ord, supp;
	ord.Read(in);
	supp.Read(in);
	const int32_t split = in.Get<int32_t>();
	Acc acc;
	ctx.Scan(0, [&](const Batch &b) { Probe(b, ord, supp, split, acc); });
	ByteWriter w;
	w.Put(acc);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Dims d = BuildDims(p);
	Acc total;
	if (d.any) {
		const int parts = ParallelParticipants();
		ByteWriter in1;
		d.cust.Write(in1);
		ByteTable ord;
		for (const auto &part : RunParallel(kOrdersTask, in1.buf, {OrdersSpec()}, parts)) {
			ByteReader r(part);
			const auto keys = r.GetVector<int64_t>();
			const auto masks = r.GetVector<uint8_t>();
			for (std::size_t i = 0; i < keys.size(); i++) {
				ord.Set(keys[i], masks[i]);
			}
		}
		ord.Finish();
		ByteWriter in2;
		ord.Write(in2);
		d.supp.Write(in2);
		in2.Put<int32_t>(Jan1996());
		for (const auto &part : RunParallel(kLineitemTask, in2.buf, {LineitemSpec()}, parts)) {
			ByteReader r(part);
			const Acc a = r.Get<Acc>();
			for (int x = 0; x < 2; x++) {
				for (int y = 0; y < 2; y++) {
					total.rev[x][y] += a.rev[x][y];
					total.cnt[x][y] += a.cnt[x][y];
				}
			}
		}
	}
	Emit(p, total, out);
}

const TaskRegistrar kOrdersReg(kOrdersTask, &OrdersTask);
const TaskRegistrar kLineitemReg(kLineitemTask, &LineitemTask);
const KernelRegistrar kRegistrar({7, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q7
