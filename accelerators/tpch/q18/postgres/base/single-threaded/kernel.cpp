// TPC-H Q18, base single-threaded PostgreSQL kernel.
#include "tpch/q18/postgres/base/q18_common.hpp"

namespace accelerator_codegen::pg_base::q18 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const int64_t threshold = BindThreshold(args);
	SumTable table;
	RunReducer reducer;
	ScanTable(LineitemSpec(), [&](const Batch &b) { reducer.Consume(b, table); });
	const Qual qual = Qualifying(table, threshold);
	if (qual.keys.empty()) {
		return;
	}
	std::vector<OrderRow> rows;
	ScanTable(OrdersSpec(qual.keys.front(), qual.keys.back()),
	          [&](const Batch &b) { CollectOrders(b, qual.keys, rows); });
	Finish(rows, qual, out);
}

const KernelRegistrar kRegistrar({18, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q18
