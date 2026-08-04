#include "accelerator_codegen/aux_state/postgres/postgres_aux_registration.hpp"

#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_incremental.hpp"
#include "accelerator_codegen/tpch_q10_maintenance_policy.hpp"
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q24_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q24_maintenance_policy.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"

namespace accelerator_codegen {

void RegisterQ10AuxAcceleratorPostgres(aux_state::PostgresAuxSession &session) {
	LoadQ10CostModelParams();
	auto &conn = session.Connection();
	static const std::vector<std::string> kCustomerColumns = {"c_custkey", "c_name", "c_acctbal", "c_nationkey",
	                                                          "c_address", "c_phone", "c_comment"};
	static const std::vector<std::string> kOrdersColumns = {"o_orderkey", "o_custkey", "o_orderdate"};
	static const std::vector<std::string> kLineitemColumns = {"l_orderkey", "l_extendedprice", "l_discount",
	                                                            "l_returnflag"};
	static const std::vector<std::string> kNationColumns = {"n_nationkey", "n_name"};
	if (!aux_state::PostgresTableHasColumns(conn, "public", "customer", kCustomerColumns) ||
	    !aux_state::PostgresTableHasColumns(conn, "public", "orders", kOrdersColumns) ||
	    !aux_state::PostgresTableHasColumns(conn, "public", "lineitem", kLineitemColumns) ||
	    !aux_state::PostgresTableHasColumns(conn, "public", "nation", kNationColumns)) {
		return;
	}

	auto &registry = session.Registry();
	registry.RegisterAcceleratorIfAbsent(kQ10AuxAcceleratorId, [&]() {
		registry.RegisterAccelerator(kQ10AuxAcceleratorId, InterpretQ10AuxTableMutation);
		registry.RegisterIncrementalBufferFactory(kQ10AuxAcceleratorId,
		                                           []() { return std::make_unique<Q10IncrementalTxnBuffer>(); });
		registry.RegisterMutationAccumulator(kQ10AuxAcceleratorId, AccumulateQ10Mutation);
		registry.RegisterMaintenanceHook(kQ10AuxAcceleratorId, CreateQ10MaintenanceHook());
		registry.RegisterTableDependency(
		    kQ10AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "customer", kCustomerColumns));
		registry.RegisterTableDependency(
		    kQ10AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "orders", kOrdersColumns));
		registry.RegisterTableDependency(
		    kQ10AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "lineitem", kLineitemColumns));
		registry.RegisterTableDependency(
		    kQ10AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "nation", kNationColumns));
	});
}

void RegisterSecEdgarQ24AuxAcceleratorPostgres(aux_state::PostgresAuxSession &session) {
	LoadSecEdgarQ24CostModelParams();
	auto &conn = session.Connection();
	static const std::vector<std::string> kNumColumns = {"adsh", "tag", "version", "uom", "value", "ddate"};
	static const std::vector<std::string> kPreColumns = {"adsh", "tag", "version"};
	if (!aux_state::PostgresTableHasColumns(conn, "public", "num", kNumColumns) ||
	    !aux_state::PostgresTableHasColumns(conn, "public", "pre", kPreColumns)) {
		return;
	}

	auto &registry = session.Registry();
	registry.RegisterAcceleratorIfAbsent(kSecEdgarQ24AuxAcceleratorId, [&]() {
		registry.RegisterAccelerator(kSecEdgarQ24AuxAcceleratorId, InterpretSecEdgarQ24AuxTableMutation);
		registry.RegisterIncrementalBufferFactory(kSecEdgarQ24AuxAcceleratorId,
		                                           []() { return std::make_unique<SecEdgarQ24IncrementalTxnBuffer>(); });
		registry.RegisterMutationAccumulator(kSecEdgarQ24AuxAcceleratorId, AccumulateSecEdgarQ24Mutation);
		registry.RegisterMaintenanceHook(kSecEdgarQ24AuxAcceleratorId, CreateSecEdgarQ24MaintenanceHook());
		registry.RegisterTableDependency(
		    kSecEdgarQ24AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "num", kNumColumns));
		registry.RegisterTableDependency(
		    kSecEdgarQ24AuxAcceleratorId, aux_state::MakePostgresTableDependency(conn, "public", "pre", kPreColumns));
	});
}

} // namespace accelerator_codegen
