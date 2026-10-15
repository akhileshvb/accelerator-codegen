// TPC-H Q22, base single-threaded PostgreSQL kernel.
#include "tpch/q22/postgres/base/q22_common.hpp"

namespace accelerator_codegen::pg_base::q22 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	CodeSet set;
	BindCodes(args, set);
	Matched matched;
	ScanTable(CustomerSpec(), [&](const Batch &b) { ConsumeCustomers(b, set, matched); });
	std::vector<int64_t> keys, bals;
	std::vector<uint8_t> slots;
	Candidates(matched, matched.positive_sum, matched.positive_count, keys, bals, slots);
	Totals t;
	AntiJoin(keys, bals, slots, t);
	Emit(set, t, out);
}

const KernelRegistrar kRegistrar({22, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q22
