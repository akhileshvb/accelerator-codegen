// TPC-H Q10, base single-threaded PostgreSQL kernel.
#include "tpch/q10/postgres/base/q10_common.hpp"

namespace accelerator_codegen::pg_base::q10 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	OrderCollector oc;
	ScanTable(OrdersSpec(p), [&](const Batch &b) { oc.Add(b); });
	if (oc.orderkey.empty()) {
		return;
	}
	const Orders o = Compact(oc.orderkey, oc.custkey);
	Accumulator acc;
	acc.Init(o.orderkey, o.cust_id, o.custkeys.size());
	ScanTable(LineitemSpec(), [&](const Batch &b) { acc.Consume(b); });
	const auto ranked = TopCustomers(acc.rev, o.custkeys);
	if (ranked.empty()) {
		return;
	}
	NationNames nn;
	ScanTable(NationSpec(), [&](const Batch &b) { nn.Consume(b); });
	CustomerPicker pick;
	for (int32_t id : ranked) {
		pick.cands.push_back(o.custkeys[id]);
	}
	std::sort(pick.cands.begin(), pick.cands.end());
	ScanTable(CustomerSpec(), [&](const Batch &b) { pick.Consume(b); });
	Emit(ranked, acc.rev, o.custkeys, pick.found, nn, out);
}

const KernelRegistrar kRegistrar({10, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q10
