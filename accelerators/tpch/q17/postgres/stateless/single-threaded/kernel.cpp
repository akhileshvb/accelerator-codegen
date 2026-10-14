// TPC-H Q17, stateless single-threaded PostgreSQL kernel.
#include "tpch/q17/postgres/stateless/q17_common.hpp"

namespace accelerator_codegen::pg_stateless::q17 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	PartSet parts;
	ScanTable(PartSpec(args.Text(0), args.Text(1)), [&](const Batch &b) { parts.Consume(b); });
	parts.Finish();
	if (parts.count == 0) {
		out.Null();
		out.EndRow();
		return;
	}
	Collector col(parts);
	ScanTable(LineitemSpec(), [&](const Batch &b) { col.Consume(b); });
	Finish(col.rows, out);
}

const KernelRegistrar kRegistrar({17, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q17
