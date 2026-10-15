// TPC-H Q4, base parallel PostgreSQL kernel (one round, index nested loop).
//   Every participant pulls batches of the orders date window from the shared
//   parallel bitmap heap scan (idx_orders_orderdate) and, per batch, probes
//   lineitem for exactly that batch's orders: a private index-driven bitmap scan
//   by l_orderkey (commitdate < receiptdate test + EXISTS marking).  Orders and
//   probe work interleave, so the participants stay balanced to the batch and
//   nothing is gathered by or broadcast from the leader.  Each order belongs to
//   exactly one batch, so the per-priority counts are simply summed by the
//   leader (order-insensitive, deterministic).
#include "tpch/q4/postgres/base/q4_common.hpp"

#include <map>

namespace accelerator_codegen::pg_base::q4 {
namespace {

constexpr const char *kTask = "tpch_q4_orders_probe";

void Task(TaskContext &ctx) {
	PrioDict dict;
	std::vector<int64_t> counts;
	std::vector<int64_t> keys;
	std::vector<uint8_t> prio;
	std::vector<uint8_t> seen;
	ctx.Scan(0, [&](const Batch &b) {
		const int64_t *k = b.I64(kOOrderkey);
		const StringRef *s = b.Str(kOPriority);
		keys.assign(k, k + b.count);
		prio.resize(b.count);
		for (std::size_t i = 0; i < b.count; i++) {
			prio[i] = dict.Id(s[i].View());
		}
		counts.resize(dict.names.size(), 0);
		KeyIndex index;
		index.Reserve(keys.size());
		for (int64_t key : keys) {
			index.Insert(key);
		}
		seen.assign(keys.size(), 0);
		ScanTable(LineitemSpec(keys), [&](const Batch &lb) { MarkExists(lb, index, seen.data()); });
		for (std::size_t i = 0; i < keys.size(); i++) {
			counts[prio[i]] += seen[i];
		}
	});
	ByteWriter w;
	w.Put<uint64_t>(dict.names.size());
	for (std::size_t i = 0; i < dict.names.size(); i++) {
		w.PutString(dict.names[i]);
		w.Put<int64_t>(counts[i]);
	}
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const auto parts = RunParallel(kTask, "", {OrdersSpec(p)}, ParallelParticipants());
	std::map<std::string, int64_t> total; // sorted by bytes
	for (const auto &part : parts) {
		ByteReader r(part);
		const auto n = r.Get<uint64_t>();
		for (uint64_t j = 0; j < n; j++) {
			std::string name = r.GetString();
			total[name] += r.Get<int64_t>();
		}
	}
	std::vector<std::string> names;
	std::vector<int64_t> counts;
	for (const auto &kv : total) {
		names.push_back(kv.first);
		counts.push_back(kv.second);
	}
	Emit(names, counts, out);
}

const TaskRegistrar kRegistrar0(kTask, &Task);
const KernelRegistrar kRegistrar({4, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q4
