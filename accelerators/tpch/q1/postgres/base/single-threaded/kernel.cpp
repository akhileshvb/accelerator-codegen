// TPC-H Q1, base single-threaded PostgreSQL kernel.
#include "tpch/q1/postgres/base/q1_common.hpp"

namespace accelerator_codegen::pg_base::q1 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	Aggregator agg;
	ScanTable(Spec(p), [&](const Batch &b) { agg.Consume(b); });
	Emit(agg.groups, out);
}

const KernelRegistrar kRegistrar({1, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q1
