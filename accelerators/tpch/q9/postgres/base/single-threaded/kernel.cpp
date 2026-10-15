// TPC-H Q9, base single-threaded PostgreSQL kernel.
#include "tpch/q9/postgres/base/q9_common.hpp"

namespace accelerator_codegen::pg_base::q9 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const std::string color = args.Text(0);
	const Dims d = BuildDims(color);
	std::vector<int64_t> ppk, psk, pc;
	ScanTable(PartsuppSpec(), [&](const Batch &b) {
		PartsuppBatch(b, d.part, [&](int64_t a, int64_t s, int64_t c) {
			ppk.push_back(a);
			psk.push_back(s);
			pc.push_back(c);
		});
	});
	PsMap ps;
	ps.Init(ppk.size());
	for (std::size_t i = 0; i < ppk.size(); i++) {
		ps.Insert(ppk[i], psk[i], pc[i]);
	}
	ByteTable ord;
	ScanTable(OrdersSpec(), [&](const Batch &b) {
		for (std::size_t i = 0; i < b.count; i++) {
			ord.Set(b.I64(0)[i], YearCode(b.I32(1)[i]));
		}
	});
	ord.Finish();
	Acc acc;
	ScanTable(LineitemSpec(), [&](const Batch &b) { Probe(b, d.part, d.supp, ord, ps, acc); });
	Emit(d, acc, out);
}

const KernelRegistrar kRegistrar({9, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q9
