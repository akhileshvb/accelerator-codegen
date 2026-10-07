// TPC-H Q6, stateless single-threaded PostgreSQL kernel.
#include "tpch/q6/postgres/stateless/q6_common.hpp"

namespace accelerator_codegen::pg_stateless::q6 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	Agg agg;
	ScanTable(Spec(p), [&](const Batch &b) { Accumulate(b, agg); });
	Emit(agg, out);
}

const KernelRegistrar kRegistrar({6, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q6
