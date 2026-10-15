// TPC-H Q11, base single-threaded PostgreSQL kernel.
#include "tpch/q11/postgres/base/q11_common.hpp"

namespace accelerator_codegen::pg_base::q11 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> suppkeys;
	const Flags sf = LoadSuppliers(p.nation, suppkeys);
	if (sf.Slots() == 0) {
		return;
	}
	State st;
	ScanTable(PartsuppSpec(suppkeys), [&](const Batch &b) { st.Consume(b, sf); });
	Finish(st.map, p, out);
}

const KernelRegistrar kRegistrar({11, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q11
