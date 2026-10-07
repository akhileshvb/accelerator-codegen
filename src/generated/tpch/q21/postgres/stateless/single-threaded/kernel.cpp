// TPC-H Q21, stateless single-threaded PostgreSQL kernel.
#include "tpch/q21/postgres/stateless/q21_common.hpp"

namespace accelerator_codegen::pg_stateless::q21 {
namespace {

void Run(const Args &args, ResultWriter &out) {
	Suppliers sup;
	if (!sup.Load(args.Text(0))) {
		return;
	}
	OrderSet os;
	ScanTable(OrdersSpec(), [&](const Batch &b) { os.Consume(b); });
	os.Build();
	std::vector<State> states(os.Slots(), InitState());
	bool bad = false;
	ScanTable(LineitemSpec(), [&](const Batch &b) { UpdateStates(b, os, states.data(), bad); });
	if (bad) {
		throw KernelError("Q21: supplier key outside int32 range");
	}
	Emit(os, states, sup, out);
}

const KernelRegistrar kRegistrar({21, false, &Run, kSqlArgs, kSqlReturns});

} // namespace
} // namespace accelerator_codegen::pg_stateless::q21
