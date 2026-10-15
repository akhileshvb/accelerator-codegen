// TPC-H Q5, base parallel PostgreSQL kernel.
// Round 0 (leader): region / nation / supplier / customer -> small dense tables.
// Round 1: parallel orders scan -> (orderkey, customer nation code) pairs; the
//          leader merges them into the dense order table.
// Round 2: parallel lineitem scan probing the order + supplier tables; each
//          participant returns its per-nation revenue sums.
#include "tpch/q5/postgres/base/q5_common.hpp"

namespace accelerator_codegen::pg_base::q5 {
namespace {

constexpr const char *kOrdersTask = "tpch_q5_orders";
constexpr const char *kLineitemTask = "tpch_q5_lineitem";

void OrdersTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable cust;
	cust.Read(in);
	std::vector<int64_t> keys;
	std::vector<uint8_t> codes;
	ctx.Scan(0, [&](const Batch &b) {
		OrdersBatch(b, cust, [&](int64_t k, uint8_t c) {
			keys.push_back(k);
			codes.push_back(c);
		});
	});
	ByteWriter w;
	w.PutVector(keys);
	w.PutVector(codes);
	ctx.SetOutput(w.Take());
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable ord, supp;
	ord.Read(in);
	supp.Read(in);
	Acc acc;
	ctx.Scan(0, [&](const Batch &b) { Probe(b, ord, supp, acc); });
	ByteWriter w;
	w.Put(acc);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Dims d = BuildDims(p);
	Acc total;
	if (!d.names.empty()) {
		const int parts = ParallelParticipants();
		ByteWriter in1;
		d.cust.Write(in1);
		ByteTable ord;
		std::vector<int64_t> all_keys;
		for (const auto &part : RunParallel(kOrdersTask, in1.buf, {OrdersSpec(p)}, parts)) {
			ByteReader r(part);
			const auto keys = r.GetVector<int64_t>();
			const auto codes = r.GetVector<uint8_t>();
			for (std::size_t i = 0; i < keys.size(); i++) {
				ord.Set(keys[i], codes[i]);
			}
			all_keys.insert(all_keys.end(), keys.begin(), keys.end());
		}
		ord.Finish();
		ByteWriter in2;
		ord.Write(in2);
		d.supp.Write(in2);
		for (const auto &part : RunParallel(kLineitemTask, in2.buf, {LineitemSpec(all_keys)}, parts)) {
			ByteReader r(part);
			const Acc a = r.Get<Acc>();
			for (int j = 0; j < 256; j++) {
				total.rev[j] += a.rev[j];
				total.cnt[j] += a.cnt[j];
			}
		}
	}
	Emit(d, total, out);
}

const TaskRegistrar kOrdersReg(kOrdersTask, &OrdersTask);
const TaskRegistrar kLineitemReg(kLineitemTask, &LineitemTask);
const KernelRegistrar kRegistrar({5, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q5
