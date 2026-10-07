// TPC-H Q19, stateless parallel PostgreSQL kernel.
// Round 1: parallel scan of part -> qualifying (partkey, group mask).
// Round 2: the leader builds the mask table and the key set; every participant scans
// its share of lineitem (IntIn on l_partkey -> index-driven bitmap scan, plus the
// common predicates) and returns a partial exact sum.
#include "tpch/q19/postgres/stateless/q19_common.hpp"

namespace accelerator_codegen::pg_stateless::q19 {
namespace {

constexpr const char *kTaskPart = "tpch_q19_part";
constexpr const char *kTask = "tpch_q19_lineitem";

void PutParams(ByteWriter &w, const Params &p) {
	for (int g = 0; g < 3; g++) {
		w.PutString(p.brand[g]);
		w.Put(p.qlo[g]);
		w.Put(p.qhi[g]);
	}
}

Params GetParams(ByteReader &in) {
	Params q;
	for (int g = 0; g < 3; g++) {
		q.brand[g] = in.GetString();
		q.qlo[g] = in.Get<int64_t>();
		q.qhi[g] = in.Get<int64_t>();
	}
	return q;
}

void TaskPart(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	std::vector<PartHit> hits;
	ctx.Scan(0, [&](const Batch &b) { PartConsume(b, p, hits); });
	ByteWriter w;
	w.PutVector(hits);
	ctx.SetOutput(w.Take());
}

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const Params p = GetParams(in);
	ByteTable part;
	part.Read(in);
	Agg agg;
	ctx.Scan(0, [&](const Batch &b) { Probe(b, p, part, agg); });
	ByteWriter w;
	w.Put(agg);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int participants = ParallelParticipants();
	ByteWriter in1;
	PutParams(in1, p);
	std::vector<PartHit> hits;
	for (const auto &o : RunParallel(kTaskPart, in1.buf, {PartSpec(p)}, participants)) {
		ByteReader r(o);
		const auto v = r.GetVector<PartHit>();
		hits.insert(hits.end(), v.begin(), v.end());
	}
	Agg total;
	if (hits.empty()) {
		Emit(total, out);
		return;
	}
	std::vector<int64_t> keys;
	const ByteTable part = MakeTable(hits, keys);
	ByteWriter in;
	PutParams(in, p);
	part.Write(in);
	for (const auto &o : RunParallel(kTask, in.buf, {LineitemSpec(p, keys)}, participants)) {
		ByteReader r(o);
		const Agg a = r.Get<Agg>();
		total.sum += a.sum;
		total.cnt += a.cnt;
	}
	Emit(total, out);
}

const TaskRegistrar kTaskRegistrar1(kTaskPart, &TaskPart);
const TaskRegistrar kTaskRegistrar2(kTask, &Task);
const KernelRegistrar kRegistrar({19, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q19
