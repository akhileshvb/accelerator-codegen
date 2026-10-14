// TPC-H Q8, stateless single-threaded PostgreSQL kernel.
#include "tpch/q8/postgres/stateless/q8_common.hpp"

namespace accelerator_codegen::pg_stateless::q8 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	const NationInfo ni = LoadNations(p);
	std::vector<int64_t> pkeys, skeys, ckeys;
	ScanTable(PartSpec(), [&](const Batch &b) { CollectParts(b, p.type_suffix, pkeys); });
	ScanTable(SupplierSpec(), [&](const Batch &b) { CollectSuppliers(b, ni.target, skeys); });
	ScanTable(CustomerSpec(), [&](const Batch &b) { CollectCustomers(b, ni, ckeys); });
	OrderRows rows;
	ScanTable(OrdersSpec(), [&](const Batch &b) { rows.Add(b); });
	std::vector<int64_t> okey;
	std::vector<uint8_t> oyear;
	FilterOrders(rows, Bitmap::Make(ckeys), okey, oyear);
	Prober pr;
	pr.Init(Bitmap::Make(pkeys), Bitmap::Make(skeys), okey, oyear);
	ScanTable(LineitemSpec(), [&](const Batch &b) { pr.Consume(b); });
	Emit(pr.agg, out);
}

const KernelRegistrar kRegistrar({8, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q8
