// TPC-H Q10, stateless parallel PostgreSQL kernel.
//   round 1: parallel orders scan (date window) -> (orderkey, custkey) rows
//   round 2: parallel lineitem scan (returnflag = 'R' pushed down); input =
//            order -> compact customer id; output = dense per-customer revenue
//   round 3: parallel customer scan picks the 20 winners' rows
#include "tpch/q10/postgres/stateless/q10_common.hpp"

namespace accelerator_codegen::pg_stateless::q10 {
namespace {

constexpr const char *kOrdersTask = "tpch_q10_orders";
constexpr const char *kLineTask = "tpch_q10_lineitem";
constexpr const char *kCustTask = "tpch_q10_customer";

void OrdersTask(TaskContext &ctx) {
	OrderCollector oc;
	ctx.Scan(0, [&](const Batch &b) { oc.Add(b); });
	ByteWriter w;
	w.PutVector(oc.orderkey);
	w.PutVector(oc.custkey);
	ctx.SetOutput(w.Take());
}

void LineTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const auto orderkey = in.GetVector<int64_t>();
	const auto cust_id = in.GetVector<int32_t>();
	const auto ncust = in.Get<uint64_t>();
	Accumulator acc;
	acc.Init(orderkey, cust_id, ncust);
	ctx.Scan(0, [&](const Batch &b) { acc.Consume(b); });
	acc.rev.resize(ncust);
	ByteWriter w;
	w.PutVector(acc.rev);
	ctx.SetOutput(w.Take());
}

void CustTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	CustomerPicker pick;
	pick.cands = in.GetVector<int64_t>();
	ctx.Scan(0, [&](const Batch &b) { pick.Consume(b); });
	ByteWriter w;
	w.Put<uint64_t>(pick.found.size());
	for (const auto &r : pick.found) {
		PutCust(w, r);
	}
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int parts = ParallelParticipants();
	std::vector<int64_t> okeys, ckeys;
	for (const auto &part : RunParallel(kOrdersTask, "", {OrdersSpec(p)}, parts)) {
		ByteReader r(part);
		auto a = r.GetVector<int64_t>();
		auto c = r.GetVector<int64_t>();
		okeys.insert(okeys.end(), a.begin(), a.end());
		ckeys.insert(ckeys.end(), c.begin(), c.end());
	}
	if (okeys.empty()) {
		return;
	}
	const Orders o = Compact(okeys, ckeys);
	ByteWriter in;
	in.PutVector(o.orderkey);
	in.PutVector(o.cust_id);
	in.Put<uint64_t>(o.custkeys.size());
	std::vector<int64_t> rev(o.custkeys.size(), 0);
	for (const auto &part : RunParallel(kLineTask, in.buf, {LineitemSpec()}, parts)) {
		ByteReader r(part);
		const auto v = r.GetVector<int64_t>();
		for (std::size_t i = 0; i < rev.size(); i++) {
			rev[i] += v[i];
		}
	}
	const auto ranked = TopCustomers(rev, o.custkeys);
	if (ranked.empty()) {
		return;
	}
	NationNames nn;
	ScanTable(NationSpec(), [&](const Batch &b) { nn.Consume(b); });
	std::vector<int64_t> cands;
	for (int32_t id : ranked) {
		cands.push_back(o.custkeys[id]);
	}
	std::sort(cands.begin(), cands.end());
	ByteWriter cin;
	cin.PutVector(cands);
	std::vector<CustRow> found;
	for (const auto &part : RunParallel(kCustTask, cin.buf, {CustomerSpec()}, parts)) {
		ByteReader r(part);
		const auto n = r.Get<uint64_t>();
		for (uint64_t i = 0; i < n; i++) {
			found.push_back(GetCust(r));
		}
	}
	Emit(ranked, rev, o.custkeys, found, nn, out);
}

const TaskRegistrar kT1(kOrdersTask, &OrdersTask);
const TaskRegistrar kT2(kLineTask, &LineTask);
const TaskRegistrar kT3(kCustTask, &CustTask);
const KernelRegistrar kRegistrar({10, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q10
