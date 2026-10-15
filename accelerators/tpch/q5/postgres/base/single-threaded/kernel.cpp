// TPC-H Q5, base single-threaded PostgreSQL kernel.
#include "tpch/q5/postgres/base/q5_common.hpp"

namespace accelerator_codegen::pg_base::q5 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Dims d = BuildDims(p);
	Acc acc;
	if (!d.names.empty()) {
		ByteTable ord;
		std::vector<int64_t> keys;
		ScanTable(OrdersSpec(p), [&](const Batch &b) {
			OrdersBatch(b, d.cust, [&](int64_t k, uint8_t c) {
				ord.Set(k, c);
				keys.push_back(k);
			});
		});
		ord.Finish();
		ScanTable(LineitemSpec(keys), [&](const Batch &b) { Probe(b, ord, d.supp, acc); });
	}
	Emit(d, acc, out);
}

const KernelRegistrar kRegistrar({5, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q5
