// TPC-H Q2, stateless parallel PostgreSQL kernel.
// Round 1: parallel scan of part (filtered by size/type) -> qualifying (partkey, mfgr).
// Round 2: parallel scan of partsupp with part + region-supplier flags broadcast; each
//          participant returns its candidates equal to its local per-part min cost.
// The leader (region/nation/supplier scanned serially) takes the global min per part.
#include "tpch/q2/postgres/stateless/q2_common.hpp"

namespace accelerator_codegen::pg_stateless::q2 {
namespace {

constexpr const char *kTaskPart = "tpch_q2_part";
constexpr const char *kTaskPartsupp = "tpch_q2_partsupp";

void TaskPart(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	bool pushed = false;
	PartSpec(p, pushed);
	std::vector<PartInfo> parts;
	ctx.Scan(0, [&](const Batch &b) { PartConsume(b, p, pushed, parts); });
	ByteWriter w;
	w.Put<uint64_t>(parts.size());
	for (const auto &pi : parts) {
		w.Put<int64_t>(pi.key);
		w.PutString(pi.mfgr);
	}
	ctx.SetOutput(w.Take());
}

void TaskPartsupp(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Flags pf = Flags::Get(in);
	const Flags sf = Flags::Get(in);
	PsState st;
	st.Init(pf.Slots());
	ctx.Scan(0, [&](const Batch &b) { st.Consume(b, pf, sf); });
	ByteWriter w;
	w.PutVector(st.Final(pf));
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
	std::vector<PartInfo> parts;
	for (const auto &part : RunParallel(kTaskPart, in1.buf, {pspec}, participants)) {
		ByteReader r(part);
		const uint64_t n = r.Get<uint64_t>();
		for (uint64_t i = 0; i < n; i++) {
			PartInfo pi;
			pi.key = r.Get<int64_t>();
			pi.mfgr = r.GetString();
			parts.push_back(std::move(pi));
		}
	}
	if (parts.empty()) {
		return;
	}
	const Flags pf = PartFlags(parts);
	ByteWriter in2;
	pf.Put(in2);
	small.flags.Put(in2);
	std::vector<int64_t> minc(pf.Slots(), INT64_MAX);
	std::vector<Cand> all;
	for (const auto &part : RunParallel(kTaskPartsupp, in2.buf, {PartsuppSpec(parts)}, participants)) {
		ByteReader r(part);
		for (const auto &c : r.GetVector<Cand>()) {
			auto &m = minc[pf.Index(c.pk)];
			m = std::min(m, c.cost);
			all.push_back(c);
		}
	}
	std::vector<Cand> fin;
	for (const auto &c : all) {
		if (c.cost == minc[pf.Index(c.pk)]) {
			fin.push_back(c);
		}
	}
	Emit(small, parts, std::move(fin), out);
}

const TaskRegistrar kTaskRegistrar1(kTaskPart, &TaskPart);
const TaskRegistrar kTaskRegistrar2(kTaskPartsupp, &TaskPartsupp);
const KernelRegistrar kRegistrar({2, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q2
