// TPC-H Q8, base parallel PostgreSQL kernel.
//   leader: region + nation (tiny serial scans)
//   round 1: parallel part / supplier / customer / orders scans -> key lists
//   leader: bitmaps + order hash inputs
//   round 2: parallel lineitem scan probing the broadcast structures -> per-year sums
#include "tpch/q8/postgres/base/q8_common.hpp"

namespace accelerator_codegen::pg_base::q8 {
namespace {

constexpr const char *kDimTask = "tpch_q8_dims";
constexpr const char *kLineTask = "tpch_q8_lineitem";

void DimTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	NationInfo ni;
	ni.in_region = in.GetVector<uint8_t>();
	ni.target = in.Get<int32_t>();
	const std::string suffix = in.GetString();
	std::vector<int64_t> pkeys, skeys, ckeys;
	OrderRows rows;
	ctx.Scan(0, [&](const Batch &b) { CollectParts(b, suffix, pkeys); });
	ctx.Scan(1, [&](const Batch &b) { CollectSuppliers(b, ni.target, skeys); });
	ctx.Scan(2, [&](const Batch &b) { CollectCustomers(b, ni, ckeys); });
	ctx.Scan(3, [&](const Batch &b) { rows.Add(b); });
	ByteWriter w;
	w.PutVector(pkeys);
	w.PutVector(skeys);
	w.PutVector(ckeys);
	w.PutVector(rows.orderkey);
	w.PutVector(rows.custkey);
	w.PutVector(rows.year);
	ctx.SetOutput(w.Take());
}

void LineTask(TaskContext &ctx) {
	ByteReader in(ctx.Input());
	Bitmap part = GetBitmap(in);
	Bitmap supp = GetBitmap(in);
	const auto okey = in.GetVector<int64_t>();
	const auto oyear = in.GetVector<uint8_t>();
	Prober pr;
	pr.Init(std::move(part), std::move(supp), okey, oyear);
	ctx.Scan(0, [&](const Batch &b) { pr.Consume(b); });
	ByteWriter w;
	w.Put(pr.agg);
	ctx.SetOutput(w.Take());
}

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const int parts = ParallelParticipants();
	const NationInfo ni = LoadNations(p);
	ByteWriter in;
	in.PutVector(ni.in_region);
	in.Put(ni.target);
	in.PutString(p.type_suffix);
	std::vector<int64_t> pkeys, skeys, ckeys;
	OrderRows rows;
	for (const auto &part :
	     RunParallel(kDimTask, in.buf, {PartSpec(), SupplierSpec(), CustomerSpec(), OrdersSpec()}, parts)) {
		ByteReader r(part);
		for (auto *dst : {&pkeys, &skeys, &ckeys}) {
			auto v = r.GetVector<int64_t>();
			dst->insert(dst->end(), v.begin(), v.end());
		}
		auto ok = r.GetVector<int64_t>();
		auto ck = r.GetVector<int64_t>();
		auto yr = r.GetVector<uint8_t>();
		rows.orderkey.insert(rows.orderkey.end(), ok.begin(), ok.end());
		rows.custkey.insert(rows.custkey.end(), ck.begin(), ck.end());
		rows.year.insert(rows.year.end(), yr.begin(), yr.end());
	}
	std::vector<int64_t> okey;
	std::vector<uint8_t> oyear;
	FilterOrders(rows, Bitmap::Make(ckeys), okey, oyear);
	ByteWriter lin;
	PutBitmap(lin, Bitmap::Make(pkeys));
	PutBitmap(lin, Bitmap::Make(skeys));
	lin.PutVector(okey);
	lin.PutVector(oyear);
	Agg total;
	for (const auto &part : RunParallel(kLineTask, lin.buf, {LineitemSpec()}, parts)) {
		ByteReader r(part);
		const Agg a = r.Get<Agg>();
		for (int y = 0; y < 2; y++) {
			total.total[y] += a.total[y];
			total.target[y] += a.target[y];
			total.rows[y] += a.rows[y];
		}
	}
	Emit(total, out);
}

const TaskRegistrar kT1(kDimTask, &DimTask);
const TaskRegistrar kT2(kLineTask, &LineTask);
const KernelRegistrar kRegistrar({8, true, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q8
