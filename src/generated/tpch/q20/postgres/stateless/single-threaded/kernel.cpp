// TPC-H Q20, stateless single-threaded PostgreSQL kernel.
#include "tpch/q20/postgres/stateless/q20_common.hpp"

namespace accelerator_codegen::pg_stateless::q20 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Small small = LoadSmall(p);
	if (small.supp.empty()) {
		return;
	}
	bool pushed = false;
	const ScanSpec pspec = PartSpec(p, pushed);
	std::vector<int64_t> parts;
	ScanTable(pspec, [&](const Batch &b) { PartConsume(b, p, pushed, parts); });
	if (parts.empty()) {
		return;
	}
	const Flags partf = KeyFlags(parts);
	std::vector<PsRow> ps, rows;
	std::vector<uint32_t> sel(kBatchRows + simd::kSelSlack);
	ScanTable(PartsuppSpec(), [&](const Batch &b) {
		const std::size_t n = SelectBothFlagged(partf, b.I64(0), small.flags, b.I64(1), b.count, sel.data());
		for (std::size_t j = 0; j < n; j++) {
			ps.push_back({b.I64(0)[sel[j]], b.I64(1)[sel[j]], b.I32(2)[sel[j]]});
		}
	});
	PairTable t;
	BuildPairs(ps, partf, rows, t);
	if (rows.empty()) {
		return;
	}
	LineState ls(rows.size());
	ScanTable(LineitemSpec(p), [&](const Batch &b) { ls.Consume(b, t); });
	Emit(small, t, rows, ls.sum, ls.seen, out);
}

const KernelRegistrar kRegistrar({20, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q20
