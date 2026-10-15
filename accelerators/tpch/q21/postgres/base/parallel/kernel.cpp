// TPC-H Q21, base parallel PostgreSQL kernel.
//   Round 1: parallel orders scan -> per-participant F-order bit vectors; the
//            leader ORs them (and scans nation/supplier serially).
//   Round 2: the merged bit vector is broadcast; every participant updates the
//            per-order states over its share of the lineitem parallel scan and
//            returns the state array; the leader merges the states
//            (associative / commutative) and runs the final pass.
#include "tpch/q21/postgres/base/q21_common.hpp"

namespace accelerator_codegen::pg_base::q21 {
namespace {

constexpr const char *kOrdersTask = "tpch_q21_orders";
constexpr const char *kLineitemTask = "tpch_q21_lineitem";

void OrdersTask(TaskContext &ctx) {
	OrderSet os;
	ctx.Scan(0, [&](const Batch &b) { os.Consume(b); });
	ByteWriter w;
	w.PutVector(os.words);
	w.PutVector(os.big);
	ctx.SetOutput(w.Take());
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	OrderSet os;
	os.words = in.GetVector<uint64_t>();
	os.big = in.GetVector<int64_t>();
	os.Build();
	std::vector<State> states(os.Slots(), InitState());
	bool bad = false;
	ctx.Scan(0, [&](const Batch &b) { UpdateStates(b, os, states.data(), bad); });
	if (bad) {
		throw KernelError("Q21: supplier key outside int32 range");
	}
	ByteWriter w;
	w.PutVector(states);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	Suppliers sup;
	if (!sup.Load(args.Text(0))) {
		return;
	}
	const int participants = ParallelParticipants();
	OrderSet os;
	for (const auto &part : RunParallel(kOrdersTask, "", {OrdersSpec()}, participants)) {
		ByteReader r(part);
		OrderSet p;
		p.words = r.GetVector<uint64_t>();
		p.big = r.GetVector<int64_t>();
		os.Merge(p);
	}
	os.Build();
	ByteWriter in;
	in.PutVector(os.words);
	in.PutVector(os.big);
	std::vector<State> states(os.Slots(), InitState());
	for (const auto &part : RunParallel(kLineitemTask, in.buf, {LineitemSpec()}, participants)) {
		ByteReader r(part);
		const std::vector<State> p = r.GetVector<State>();
		if (p.size() != states.size()) {
			throw KernelError("Q21: partial state size mismatch");
		}
		for (std::size_t i = 0; i < p.size(); i++) {
			MergeState(states[i], p[i]);
		}
	}
	Emit(os, states, sup, out);
}

const TaskRegistrar kOrdersRegistrar(kOrdersTask, &OrdersTask);
const TaskRegistrar kLineitemRegistrar(kLineitemTask, &LineitemTask);
const KernelRegistrar kRegistrar({21, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q21
