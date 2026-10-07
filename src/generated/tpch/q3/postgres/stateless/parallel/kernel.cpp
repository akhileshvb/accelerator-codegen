// TPC-H Q3, stateless parallel PostgreSQL kernel.
//   round 0 (leader, serial): customer scan (small) -> custkey list.
//   round 1: custkeys broadcast; participants scan their share of orders and
//            return the qualifying orders (key, date, shippriority); the leader
//            concatenates them into one order list.
//   round 2: the order keys are broadcast; participants index them, scan their
//            share of lineitem and return per-order revenue + hit vectors
//            (indexed by list position); the leader adds them and picks the top 10.
#include "tpch/q3/postgres/stateless/q3_common.hpp"

namespace accelerator_codegen::pg_stateless::q3 {
namespace {

constexpr const char *kOrdersTask = "tpch_q3_orders";
constexpr const char *kLineitemTask = "tpch_q3_lineitem";

void OrdersTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const std::vector<int64_t> custkeys = in.GetVector<int64_t>();
	const KeyIndex customers = BuildCustomerSet(custkeys);
	OrderList orders;
	ctx.Scan(0, [&](const Batch &b) { CollectOrders(b, customers, orders); });
	ByteWriter w;
	w.PutVector(orders.keys);
	w.PutVector(orders.dates);
	w.PutVector(orders.prio);
	ctx.SetOutput(w.Take());
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const std::vector<int64_t> keys = in.GetVector<int64_t>();
	KeyIndex index;
	index.Reserve(keys.size());
	for (int64_t k : keys) {
		index.Insert(k);
	}
	Revenue rev;
	rev.Init(keys.size());
	ctx.Scan(0, [&](const Batch &b) { ProbeLineitem(b, index, rev); });
	ByteWriter w;
	w.PutVector(rev.sum);
	w.PutVector(rev.hit);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int participants = ParallelParticipants();
	std::vector<int64_t> custkeys;
	ScanTable(CustomerSpec(p), [&](const Batch &b) { CollectCustomers(b, custkeys); });
	if (custkeys.empty()) {
		return;
	}
	ByteWriter in1;
	in1.PutVector(custkeys);
	OrderList orders;
	for (const auto &part : RunParallel(kOrdersTask, in1.buf, {OrdersSpec(p)}, participants)) {
		ByteReader r(part);
		const auto k = r.GetVector<int64_t>();
		const auto d = r.GetVector<int32_t>();
		const auto s = r.GetVector<int32_t>();
		orders.keys.insert(orders.keys.end(), k.begin(), k.end());
		orders.dates.insert(orders.dates.end(), d.begin(), d.end());
		orders.prio.insert(orders.prio.end(), s.begin(), s.end());
	}
	if (orders.keys.empty()) {
		return;
	}
	ByteWriter in2;
	in2.PutVector(orders.keys);
	Revenue rev;
	rev.Init(orders.keys.size());
	for (const auto &part : RunParallel(kLineitemTask, in2.buf, {LineitemSpec(p)}, participants)) {
		ByteReader r(part);
		const auto sum = r.GetVector<int64_t>();
		const auto hit = r.GetVector<uint8_t>();
		for (std::size_t i = 0; i < sum.size(); i++) {
			rev.sum[i] += sum[i];
			rev.hit[i] |= hit[i];
		}
	}
	Emit(orders, rev, out);
}

const TaskRegistrar kOrdersRegistrar(kOrdersTask, &OrdersTask);
const TaskRegistrar kLineitemRegistrar(kLineitemTask, &LineitemTask);
const KernelRegistrar kRegistrar({3, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q3
