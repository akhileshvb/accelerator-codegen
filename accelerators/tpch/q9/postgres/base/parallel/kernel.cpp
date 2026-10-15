// TPC-H Q9, base parallel PostgreSQL kernel.
// Round 0 (leader): part (LIKE), nation, supplier -> small tables.
// Round 1: parallel partsupp scan (rows of matching parts) + parallel orders
//          scan (orderkey -> year code); the leader merges both.
// Round 2: parallel lineitem scan probing everything; per-(nation,year) sums
//          are merged by the leader.
#include "tpch/q9/postgres/base/q9_common.hpp"

namespace accelerator_codegen::pg_base::q9 {
namespace {

constexpr const char *kBuildTask = "tpch_q9_build";
constexpr const char *kLineitemTask = "tpch_q9_lineitem";

void BuildTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable part;
	part.Read(in);
	std::vector<int64_t> ppk, psk, pc, okeys;
	std::vector<uint8_t> years;
	ctx.Scan(0, [&](const Batch &b) {
		PartsuppBatch(b, part, [&](int64_t a, int64_t s, int64_t c) {
			ppk.push_back(a);
			psk.push_back(s);
			pc.push_back(c);
		});
	});
	ctx.Scan(1, [&](const Batch &b) {
		for (std::size_t i = 0; i < b.count; i++) {
			okeys.push_back(b.I64(0)[i]);
			years.push_back(YearCode(b.I32(1)[i]));
		}
	});
	ByteWriter w;
	w.PutVector(ppk);
	w.PutVector(psk);
	w.PutVector(pc);
	w.PutVector(okeys);
	w.PutVector(years);
	ctx.SetOutput(w.Take());
}

void LineitemTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	ByteTable part, supp, ord;
	part.Read(in);
	supp.Read(in);
	ord.Read(in);
	const auto ppk = in.GetVector<int64_t>();
	const auto psk = in.GetVector<int64_t>();
	const auto pc = in.GetVector<int64_t>();
	PsMap ps;
	ps.Init(ppk.size());
	for (std::size_t i = 0; i < ppk.size(); i++) {
		ps.Insert(ppk[i], psk[i], pc[i]);
	}
	Acc acc;
	ctx.Scan(0, [&](const Batch &b) { Probe(b, part, supp, ord, ps, acc); });
	ByteWriter w;
	w.Put(acc);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const std::string color = args.Text(0);
	const Dims d = BuildDims(color);
	const int parts = ParallelParticipants();
	ByteWriter in1;
	d.part.Write(in1);
	std::vector<int64_t> ppk, psk, pc;
	ByteTable ord;
	for (const auto &part : RunParallel(kBuildTask, in1.buf, {PartsuppSpec(), OrdersSpec()}, parts)) {
		ByteReader r(part);
		const auto a = r.GetVector<int64_t>();
		const auto s = r.GetVector<int64_t>();
		const auto c = r.GetVector<int64_t>();
		const auto ok = r.GetVector<int64_t>();
		const auto yr = r.GetVector<uint8_t>();
		ppk.insert(ppk.end(), a.begin(), a.end());
		psk.insert(psk.end(), s.begin(), s.end());
		pc.insert(pc.end(), c.begin(), c.end());
		for (std::size_t i = 0; i < ok.size(); i++) {
			ord.Set(ok[i], yr[i]);
		}
	}
	ord.Finish();
	ByteWriter in2;
	d.part.Write(in2);
	d.supp.Write(in2);
	ord.Write(in2);
	in2.PutVector(ppk);
	in2.PutVector(psk);
	in2.PutVector(pc);
	Acc total;
	for (const auto &part : RunParallel(kLineitemTask, in2.buf, {LineitemSpec()}, parts)) {
		ByteReader r(part);
		const Acc a = r.Get<Acc>();
		for (int n = 0; n < kMaxNations; n++) {
			for (int y = 0; y < kMaxYears; y++) {
				total.sum[n][y] += a.sum[n][y];
				total.cnt[n][y] += a.cnt[n][y];
			}
		}
	}
	Emit(d, total, out);
}

const TaskRegistrar kBuildReg(kBuildTask, &BuildTask);
const TaskRegistrar kLineitemReg(kLineitemTask, &LineitemTask);
const KernelRegistrar kRegistrar({9, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q9
