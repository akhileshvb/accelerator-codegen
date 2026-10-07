// TPC-H Q18, stateless parallel PostgreSQL kernel.
//   round 1: every participant aggregates SUM(l_quantity) per orderkey over its
//            share of lineitem (run reduction + open addressing) and returns the
//            table in compact varint form; the leader adds the partial tables
//            (an order may straddle participants, so the HAVING is applied only
//            after the merge) and keeps the qualifying orders.
//   round 2: the qualifying keys are broadcast; participants scan their share of
//            orders and return the matching rows; the leader scans customer
//            (serial, range-filtered) for the names and sorts.
#include "tpch/q18/postgres/stateless/q18_common.hpp"

namespace accelerator_codegen::pg_stateless::q18 {
namespace {

constexpr const char *kLineitemTask = "tpch_q18_lineitem";
constexpr const char *kOrdersTask = "tpch_q18_orders";

void LineitemTask(TaskContext &ctx) {
	SumTable table;
	RunReducer reducer;
	ctx.Scan(0, [&](const Batch &b) { reducer.Consume(b, table); });
	ctx.SetOutput(EncodeTable(table));
}

void OrdersTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const std::vector<int64_t> keys = in.GetVector<int64_t>();
	std::vector<OrderRow> rows;
	ctx.Scan(0, [&](const Batch &b) { CollectOrders(b, keys, rows); });
	ByteWriter w;
	w.PutVector(rows);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const int64_t threshold = BindThreshold(args);
	const int participants = ParallelParticipants();
	SumTable table;
	for (const auto &part : RunParallel(kLineitemTask, "", {LineitemSpec()}, participants)) {
		DecodeInto(part, table);
	}
	const Qual qual = Qualifying(table, threshold);
	if (qual.keys.empty()) {
		return;
	}
	ByteWriter in;
	in.PutVector(qual.keys);
	std::vector<OrderRow> rows;
	for (const auto &part : RunParallel(kOrdersTask, in.buf,
	                                    {OrdersSpec(qual.keys.front(), qual.keys.back())}, participants)) {
		ByteReader r(part);
		const auto v = r.GetVector<OrderRow>();
		rows.insert(rows.end(), v.begin(), v.end());
	}
	Finish(rows, qual, out);
}

const TaskRegistrar kLineitemRegistrar(kLineitemTask, &LineitemTask);
const TaskRegistrar kOrdersRegistrar(kOrdersTask, &OrdersTask);
const KernelRegistrar kRegistrar({18, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q18
