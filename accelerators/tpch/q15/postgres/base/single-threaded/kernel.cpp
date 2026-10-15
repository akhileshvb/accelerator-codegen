// TPC-H Q15, base single-threaded PostgreSQL kernel.
#include "tpch/q15/postgres/base/q15_common.hpp"

namespace accelerator_codegen::pg_base::q15 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	uint64_t dense_size = 0;
	std::vector<Supplier> suppliers = ScanSuppliers(dense_size);
	Revenue rev(dense_size);
	ScanTable(LineitemSpec(p), [&](const Batch &b) { rev.Consume(b); });
	Emit(rev, suppliers, out);
}

const KernelRegistrar kRegistrar({15, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q15
