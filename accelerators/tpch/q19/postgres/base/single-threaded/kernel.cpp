// TPC-H Q19, base single-threaded PostgreSQL kernel.
#include "tpch/q19/postgres/base/q19_common.hpp"

namespace accelerator_codegen::pg_base::q19 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> keys;
	const ByteTable part = BuildPartTable(p, keys);
	Agg agg;
	if (keys.empty()) {
		Emit(agg, out);
		return;
	}
	ScanTable(LineitemSpec(p, keys), [&](const Batch &b) { Probe(b, p, part, agg); });
	Emit(agg, out);
}

const KernelRegistrar kRegistrar({19, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q19
