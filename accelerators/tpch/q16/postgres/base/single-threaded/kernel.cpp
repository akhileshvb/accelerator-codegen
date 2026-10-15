// TPC-H Q16, base single-threaded PostgreSQL kernel.
#include "tpch/q16/postgres/base/q16_common.hpp"

namespace accelerator_codegen::pg_base::q16 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> bad;
	ScanTable(SupplierSpec(), [&](const Batch &b) { SupplierConsume(b, bad); });
	std::vector<PartRow> parts;
	ScanTable(PartSpec(p), [&](const Batch &b) { PartConsume(b, p, parts); });
	if (parts.empty()) {
		return;
	}
	const PartMap pm = BuildPartMap(parts);
	const Flags badf = BadFlags(bad);
	PsState st;
	ScanTable(PartsuppSpec(), [&](const Batch &b) { st.Consume(b, pm, badf); });
	st.SortUnique();
	Emit(pm, st.pairs, out);
}

const KernelRegistrar kRegistrar({16, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q16
