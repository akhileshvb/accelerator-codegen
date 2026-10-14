// TPC-H Q17, stateless parallel PostgreSQL kernel.  The leader scans the
// (small, pre-filtered) part table and broadcasts the qualifying-key bit
// vector; every participant collects the lineitem rows of qualifying parts from
// its share of the parallel scan; the leader evaluates the per-part average.
#include "tpch/q17/postgres/stateless/q17_common.hpp"

namespace accelerator_codegen::pg_stateless::q17 {
namespace {

constexpr const char *kTask = "tpch_q17_lineitem";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const int64_t max_dense = in.Get<int64_t>();
	const std::vector<uint64_t> words = in.GetVector<uint64_t>();
	const std::vector<int64_t> big = in.GetVector<int64_t>();
	Collector col(words.data(), max_dense, big.data(), big.size());
	ctx.Scan(0, [&](const Batch &b) { col.Consume(b); });
	ByteWriter w;
	w.PutVector(col.rows);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	PartSet parts;
	ScanTable(PartSpec(args.Text(0), args.Text(1)), [&](const Batch &b) { parts.Consume(b); });
	parts.Finish();
	if (parts.count == 0) {
		out.Null();
		out.EndRow();
		return;
	}
	ByteWriter in;
	in.Put<int64_t>(parts.max_dense);
	in.PutVector(parts.words);
	in.PutVector(parts.big);
	std::vector<Row> rows;
	for (const auto &part : RunParallel(kTask, in.buf, {LineitemSpec()}, ParallelParticipants())) {
		ByteReader r(part);
		const auto v = r.GetVector<Row>();
		rows.insert(rows.end(), v.begin(), v.end());
	}
	Finish(rows, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({17, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q17
