// TPC-H Q4, base single-threaded PostgreSQL kernel.
#include "tpch/q4/postgres/base/q4_common.hpp"

namespace accelerator_codegen::pg_base::q4 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	const Params p = Bind(args);
	std::vector<int64_t> keys;
	std::vector<uint8_t> prio;
	PrioDict dict;
	ScanTable(OrdersSpec(p), [&](const Batch &b) {
		const int64_t *k = b.I64(kOOrderkey);
		const StringRef *s = b.Str(kOPriority);
		for (std::size_t i = 0; i < b.count; i++) {
			keys.push_back(k[i]);
			prio.push_back(dict.Id(s[i].View()));
		}
	});
	if (keys.empty()) {
		return;
	}
	KeyIndex index;
	index.Reserve(keys.size());
	for (int64_t k : keys) {
		index.Insert(k);
	}
	std::vector<uint8_t> seen(keys.size(), 0);
	ScanTable(LineitemSpec(keys), [&](const Batch &b) { MarkExists(b, index, seen.data()); });
	std::vector<int64_t> counts(dict.names.size(), 0);
	for (std::size_t i = 0; i < keys.size(); i++) {
		counts[prio[i]] += seen[i];
	}
	Emit(dict.names, counts, out);
}

const KernelRegistrar kRegistrar({4, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_base::q4
