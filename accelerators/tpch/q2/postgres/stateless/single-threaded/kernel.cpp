// TPC-H Q2, stateless single-threaded PostgreSQL kernel.
#include "tpch/q2/postgres/stateless/q2_common.hpp"

namespace accelerator_codegen::pg_stateless::q2 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Small small = LoadSmall(p);
	if (small.supp.empty()) {
		return;
	}
	bool pushed = false;
	const ScanSpec pspec = PartSpec(p, pushed);
	std::vector<PartInfo> parts;
	ScanTable(pspec, [&](const Batch &b) { PartConsume(b, p, pushed, parts); });
	if (parts.empty()) {
		return;
	}
	const Flags pf = PartFlags(parts);
	PsState st;
	st.Init(pf.Slots());
	ScanTable(PartsuppSpec(parts), [&](const Batch &b) { st.Consume(b, pf, small.flags); });
	Emit(small, parts, st.Final(pf), out);
}

const KernelRegistrar kRegistrar({2, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q2
