// TPC-H Q13, stateless single-threaded PostgreSQL kernel.
#include "tpch/q13/postgres/stateless/q13_common.hpp"

namespace accelerator_codegen::pg_stateless::q13 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Matcher matcher(args.Text(0));
	Customers cust;
	ScanTable(CustomerSpec(), [&](const Batch &b) { cust.Consume(b); });
	cust.Check();
	std::vector<uint32_t> counts(static_cast<std::size_t>(cust.max_key + 1), 0);
	ScanTable(OrdersSpec(), [&](const Batch &b) { CountOrders(b, matcher, cust.max_key, counts.data()); });
	Emit(cust, counts.data(), out);
}

const KernelRegistrar kRegistrar({13, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q13
