// TPC-H Q11, base parallel PostgreSQL kernel: the nation suppliers are found serially
// in the leader and broadcast as key flags; every participant aggregates its share of the
// partsupp parallel scan per part and returns the (partkey, sum) pairs; the leader adds them.
#include "tpch/q11/postgres/base/q11_common.hpp"

namespace accelerator_codegen::pg_base::q11 {
namespace {

constexpr const char *kTask = "tpch_q11_partsupp";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Flags sf = Flags::Get(in);
	State st;
	ctx.Scan(0, [&](const Batch &b) { st.Consume(b, sf); });
	std::vector<int64_t> keys;
	std::vector<__int128> sums;
	st.map.ForEach([&](int64_t k, __int128 v) {
		keys.push_back(k);
		sums.push_back(v);
	});
	ByteWriter w;
	w.PutVector(keys);
	w.PutVector(sums);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> suppkeys;
	const Flags sf = LoadSuppliers(p.nation, suppkeys);
	if (sf.Slots() == 0) {
		return;
	}
	ByteWriter in;
	sf.Put(in);
	AggMap merged;
	for (const auto &part : RunParallel(kTask, in.buf, {PartsuppSpec(suppkeys)}, ParallelParticipants())) {
		ByteReader r(part);
		const auto keys = r.GetVector<int64_t>();
		const auto sums = r.GetVector<__int128>();
		for (std::size_t i = 0; i < keys.size(); i++) {
			merged.Add(keys[i], sums[i]);
		}
	}
	Finish(merged, p, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({11, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q11
