// TPC-H Q14, stateless single-threaded PostgreSQL kernel.
#include "tpch/q14/postgres/stateless/q14_common.hpp"

namespace accelerator_codegen::pg_stateless::q14 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const PartClass pc = BuildPartClass();
	Agg agg;
	ScanTable(LineitemSpec(p), [&](const Batch &b) { Accumulate(b, pc, agg); });
	Emit(agg, out);
}

const KernelRegistrar kRegistrar({14, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q14
