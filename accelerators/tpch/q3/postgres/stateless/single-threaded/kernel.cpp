// TPC-H Q3, stateless single-threaded PostgreSQL kernel.
#include "tpch/q3/postgres/stateless/q3_common.hpp"

namespace accelerator_codegen::pg_stateless::q3 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> custkeys;
	ScanTable(CustomerSpec(p), [&](const Batch &b) { CollectCustomers(b, custkeys); });
	if (custkeys.empty()) {
		return;
	}
	const KeyIndex customers = BuildCustomerSet(custkeys);
	OrderList orders;
	ScanTable(OrdersSpec(p), [&](const Batch &b) { CollectOrders(b, customers, orders); });
	if (orders.keys.empty()) {
		return;
	}
	KeyIndex index;
	index.Reserve(orders.keys.size());
	for (int64_t k : orders.keys) {
		index.Insert(k);
	}
	Revenue rev;
	rev.Init(orders.keys.size());
	ScanTable(LineitemSpec(p), [&](const Batch &b) { ProbeLineitem(b, index, rev); });
	Emit(orders, rev, out);
}

const KernelRegistrar kRegistrar({3, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q3
