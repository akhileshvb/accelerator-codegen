// TPC-H Q7, base single-threaded PostgreSQL kernel.
#include "tpch/q7/postgres/base/q7_common.hpp"

namespace accelerator_codegen::pg_base::q7 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Dims d = BuildDims(p);
	Acc acc;
	if (d.any) {
		ByteTable ord;
		ScanTable(OrdersSpec(), [&](const Batch &b) {
			OrdersBatch(b, d.cust, [&](int64_t k, uint8_t m) { ord.Set(k, m); });
		});
		ord.Finish();
		const int32_t split = Jan1996();
		ScanTable(LineitemSpec(), [&](const Batch &b) { Probe(b, ord, d.supp, split, acc); });
	}
	Emit(p, acc, out);
}

const KernelRegistrar kRegistrar({7, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q7
