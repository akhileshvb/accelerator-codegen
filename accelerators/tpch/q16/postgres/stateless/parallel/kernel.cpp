// TPC-H Q16, stateless parallel PostgreSQL kernel.
// Round 1: parallel scans of part (filtered rows) and supplier (complaint suppliers).
// Round 2: parallel scan of partsupp with the partkey->group map and bad-supplier flags
//          broadcast; each participant returns its sorted unique (group, suppkey) pairs;
//          the leader merges, deduplicates and counts per group.
#include "tpch/q16/postgres/stateless/q16_common.hpp"

namespace accelerator_codegen::pg_stateless::q16 {
namespace {

constexpr const char *kTaskBuild = "tpch_q16_build";
constexpr const char *kTaskProbe = "tpch_q16_probe";

void TaskBuild(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	std::vector<PartRow> parts;
	std::vector<int64_t> bad;
	ctx.Scan(0, [&](const Batch &b) { PartConsume(b, p, parts); });
	ctx.Scan(1, [&](const Batch &b) { SupplierConsume(b, bad); });
	ByteWriter w;
	w.PutVector(bad);
	w.Put<uint64_t>(parts.size());
	for (const auto &pr : parts) {
		w.Put<int64_t>(pr.key);
		w.PutString(pr.brand);
		w.PutString(pr.type);
		w.Put<int32_t>(pr.size);
	}
	ctx.SetOutput(w.Take());
}

void TaskProbe(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const PartMap pm = PartMap::Get(in);
	const Flags bad = Flags::Get(in);
	PsState st;
	ctx.Scan(0, [&](const Batch &b) { st.Consume(b, pm, bad); });
	st.SortUnique();
	ByteWriter w;
	w.PutVector(st.pairs);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int participants = ParallelParticipants();
	ByteWriter in1;
	PutParams(in1, p);
	std::vector<PartRow> parts;
	std::vector<int64_t> bad;
	for (const auto &o : RunParallel(kTaskBuild, in1.buf, {PartSpec(p), SupplierSpec()}, participants)) {
		ByteReader r(o);
		const auto b = r.GetVector<int64_t>();
		bad.insert(bad.end(), b.begin(), b.end());
		const uint64_t n = r.Get<uint64_t>();
		for (uint64_t i = 0; i < n; i++) {
			PartRow pr;
			pr.key = r.Get<int64_t>();
			pr.brand = r.GetString();
			pr.type = r.GetString();
			pr.size = r.Get<int32_t>();
			parts.push_back(std::move(pr));
		}
	}
	if (parts.empty()) {
		return;
	}
	// Group ids depend on discovery order; make it deterministic.
	std::sort(parts.begin(), parts.end(), [](const PartRow &a, const PartRow &b) { return a.key < b.key; });
	const PartMap pm = BuildPartMap(parts);
	const Flags badf = BadFlags(bad);
	ByteWriter in2;
	pm.Put(in2);
	badf.Put(in2);
	std::vector<Pair> pairs;
	for (const auto &o : RunParallel(kTaskProbe, in2.buf, {PartsuppSpec()}, participants)) {
		ByteReader r(o);
		const auto v = r.GetVector<Pair>();
		pairs.insert(pairs.end(), v.begin(), v.end());
	}
	std::sort(pairs.begin(), pairs.end());
	pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
	Emit(pm, pairs, out);
}

const TaskRegistrar kTaskRegistrar1(kTaskBuild, &TaskBuild);
const TaskRegistrar kTaskRegistrar2(kTaskProbe, &TaskProbe);
const KernelRegistrar kRegistrar({16, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q16
