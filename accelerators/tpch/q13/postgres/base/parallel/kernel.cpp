// TPC-H Q13, base parallel PostgreSQL kernel.  The leader scans customer
// (small) serially and broadcasts the pattern and the largest key; every
// participant counts qualifying orders per customer key over its share of the
// orders parallel scan; the leader adds the dense count arrays and builds the
// histogram.
#include "tpch/q13/postgres/base/q13_common.hpp"

namespace accelerator_codegen::pg_base::q13 {
namespace {

constexpr const char *kTask = "tpch_q13_orders";

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	const int64_t max_key = in.Get<int64_t>();
	const Matcher matcher(in.GetString());
	std::vector<uint32_t> counts(static_cast<std::size_t>(max_key + 1), 0);
	ctx.Scan(0, [&](const Batch &b) { CountOrders(b, matcher, max_key, counts.data()); });
	ByteWriter w;
	w.PutVector(counts);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const std::string pattern = args.Text(0);
	Customers cust;
	ScanTable(CustomerSpec(), [&](const Batch &b) { cust.Consume(b); });
	cust.Check();
	ByteWriter in;
	in.Put<int64_t>(cust.max_key);
	in.PutString(pattern);
	std::vector<uint32_t> counts(static_cast<std::size_t>(cust.max_key + 1), 0);
	for (const auto &part : RunParallel(kTask, in.buf, {OrdersSpec()}, ParallelParticipants())) {
		ByteReader r(part);
		const auto c = r.GetVector<uint32_t>();
		if (c.size() != counts.size()) {
			throw KernelError("Q13: partial count size mismatch");
		}
		for (std::size_t i = 0; i < c.size(); i++) {
			counts[i] += c[i];
		}
	}
	Emit(cust, counts.data(), out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const KernelRegistrar kRegistrar({13, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q13
