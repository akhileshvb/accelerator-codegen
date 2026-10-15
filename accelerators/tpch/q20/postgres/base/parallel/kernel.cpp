// TPC-H Q20, base parallel PostgreSQL kernel.
// Round 1: parallel scans of part (name filter) and partsupp (rows of nation suppliers,
//          selected with broadcast supplier flags); the leader joins them into the candidate
//          (partkey, suppkey) pair table.
// Round 2: parallel scan of lineitem (shipdate filter in the scan); each participant sums
//          l_quantity per candidate pair (SIMD flag pre-filter + hash probe) and returns the
//          per-pair sums; the leader adds them elementwise.
#include "tpch/q20/postgres/base/q20_common.hpp"

namespace accelerator_codegen::pg_base::q20 {
namespace {

constexpr const char *kTaskBuild = "tpch_q20_build";
constexpr const char *kTaskLine = "tpch_q20_lineitem";

void TaskBuild(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	const Flags sf = Flags::Get(in);
	bool pushed = false;
	PartSpec(p, pushed);
	std::vector<int64_t> parts;
	std::vector<PsRow> ps;
	std::vector<uint32_t> sel(kBatchRows + simd::kSelSlack);
	ctx.Scan(0, [&](const Batch &b) { PartConsume(b, p, pushed, parts); });
	ctx.Scan(1, [&](const Batch &b) { PartsuppConsume(b, sf, sel, ps); });
	ByteWriter w;
	w.PutVector(parts);
	w.PutVector(ps);
	ctx.SetOutput(w.Take());
}

void TaskLine(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	PairTable t;
	t.pk = in.GetVector<int64_t>();
	t.sk = in.GetVector<int64_t>();
	t.Build();
	LineState ls(t.pk.size());
	ctx.Scan(0, [&](const Batch &b) { ls.Consume(b, t); });
	ByteWriter w;
	w.PutVector(ls.sum);
	w.PutVector(ls.seen);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const Small small = LoadSmall(p);
	if (small.supp.empty()) {
		return;
	}
	const int participants = ParallelParticipants();
	bool pushed = false;
	const ScanSpec pspec = PartSpec(p, pushed);
	ByteWriter in1;
	PutParams(in1, p);
	small.flags.Put(in1);
	std::vector<int64_t> parts;
	std::vector<PsRow> ps;
	for (const auto &o : RunParallel(kTaskBuild, in1.buf, {pspec, PartsuppSpec()}, participants)) {
		ByteReader r(o);
		const auto a = r.GetVector<int64_t>();
		parts.insert(parts.end(), a.begin(), a.end());
		const auto b = r.GetVector<PsRow>();
		ps.insert(ps.end(), b.begin(), b.end());
	}
	if (parts.empty()) {
		return;
	}
	const Flags partf = KeyFlags(parts);
	std::vector<PsRow> rows;
	PairTable t;
	BuildPairs(ps, partf, rows, t);
	if (rows.empty()) {
		return;
	}
	ByteWriter in2;
	in2.PutVector(t.pk);
	in2.PutVector(t.sk);
	std::vector<int64_t> sum(rows.size(), 0);
	std::vector<uint8_t> seen(rows.size(), 0);
	for (const auto &o : RunParallel(kTaskLine, in2.buf, {LineitemSpec(p)}, participants)) {
		ByteReader r(o);
		const auto s = r.GetVector<int64_t>();
		const auto sn = r.GetVector<uint8_t>();
		for (std::size_t i = 0; i < rows.size(); i++) {
			sum[i] += s[i];
			seen[i] |= sn[i];
		}
	}
	Emit(small, t, rows, sum, seen, out);
}

const TaskRegistrar kTaskRegistrar1(kTaskBuild, &TaskBuild);
const TaskRegistrar kTaskRegistrar2(kTaskLine, &TaskLine);
const KernelRegistrar kRegistrar({20, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q20
