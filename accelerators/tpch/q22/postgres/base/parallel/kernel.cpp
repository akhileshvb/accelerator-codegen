// TPC-H Q22, base parallel PostgreSQL kernel: one round; every participant
// scans its share of customer (matching customers) and orders (has-orders bit
// vector); the leader merges the partial results and runs the final pass.
#include "tpch/q22/postgres/base/q22_common.hpp"

namespace accelerator_codegen::pg_base::q22 {
namespace {

constexpr const char *kTask = "tpch_q22_scan";
constexpr const char *kTask2 = "tpch_q22_probe";

void ReadCodes(ByteReader &in, CodeSet &set) {
	set.n = in.Get<int>();
	for (int i = 0; i < set.n; i++) {
		set.codes[i][0] = in.Get<char>();
		set.codes[i][1] = in.Get<char>();
		const unsigned key = static_cast<unsigned char>(set.codes[i][0]) |
		                     (static_cast<unsigned>(static_cast<unsigned char>(set.codes[i][1])) << 8);
		set.slot_of[key] = static_cast<uint8_t>(i + 1);
	}
}

// Round 2: customers above the average (pushed into the scan) -> anti-join on this participant's share.
void ProbeTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	CodeSet set;
	ReadCodes(in, set);
	Matched matched;
	ctx.Scan(0, [&](const Batch &b) { ConsumeCustomers(b, set, matched); });
	Totals t;
	AntiJoin(matched.custkey, matched.acctbal, matched.slot, t);
	ByteWriter w;
	w.Put(t);
	ctx.SetOutput(w.Take());
}

void Task(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	CodeSet set;
	set.n = in.Get<int>();
	for (int i = 0; i < set.n; i++) {
		set.codes[i][0] = in.Get<char>();
		set.codes[i][1] = in.Get<char>();
		const unsigned key = static_cast<unsigned char>(set.codes[i][0]) |
		                     (static_cast<unsigned>(static_cast<unsigned char>(set.codes[i][1])) << 8);
		set.slot_of[key] = static_cast<uint8_t>(i + 1);
	}
	Matched matched;
	ctx.Scan(0, [&](const Batch &b) { ConsumeCustomers(b, set, matched); });
	ByteWriter w;
	w.Put(matched.positive_sum);
	w.Put(matched.positive_count);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	CodeSet set;
	BindCodes(args, set);
	ByteWriter in;
	in.Put<int>(set.n);
	for (int i = 0; i < set.n; i++) {
		in.Put(set.codes[i][0]);
		in.Put(set.codes[i][1]);
	}
	Matched matched;
	for (const auto &part : RunParallel(kTask, in.buf, {CustomerSpec()}, ParallelParticipants())) {
		ByteReader r(part);
		matched.positive_sum += r.Get<__int128>();
		matched.positive_count += r.Get<int64_t>();
	}
	Totals t;
	if (matched.positive_count > 0) {
		// bal * cnt > sum  <=>  bal > floor(sum / cnt)   (sum >= 0)
		const int64_t thr = static_cast<int64_t>(matched.positive_sum / matched.positive_count);
		ScanSpec spec = CustomerSpec();
		spec.filters[0].lo = thr + 1;
		for (const auto &part : RunParallel(kTask2, in.buf, {spec}, ParallelParticipants())) {
			ByteReader r(part);
			const Totals p = r.Get<Totals>();
			for (int i = 0; i < kMaxCodes; i++) {
				t.counts[i] += p.counts[i];
				t.sums[i] += p.sums[i];
			}
		}
	}
	Emit(set, t, out);
}

const TaskRegistrar kTaskRegistrar(kTask, &Task);
const TaskRegistrar kTaskRegistrar2(kTask2, &ProbeTask);
const KernelRegistrar kRegistrar({22, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q22
