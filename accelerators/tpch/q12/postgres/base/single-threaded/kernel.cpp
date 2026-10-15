// TPC-H Q12, base single-threaded PostgreSQL kernel.
#include "tpch/q12/postgres/base/q12_common.hpp"

namespace accelerator_codegen::pg_base::q12 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	Hits hits;
	ScanTable(LineitemSpec(p), [&](const Batch &b) { CollectLineitem(b, p, hits); });
	Totals t = {};
	if (!hits.keys.empty()) {
		OrderProbe probe;
		probe.Build(hits);
		ScanTable(OrdersSpec(), [&](const Batch &b) { ProbeOrders(b, probe, t); });
	}
	Emit(p, t, out);
}

const KernelRegistrar kRegistrar({12, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q12
