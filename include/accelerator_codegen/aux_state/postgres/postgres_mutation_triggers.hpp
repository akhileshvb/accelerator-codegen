#pragma once

#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"
#include "accelerator_codegen/aux_state/txn_lifecycle_backend.hpp"

namespace accelerator_codegen::aux_state {

//! Installs PostgreSQL row triggers that populate accel_mutation_log for Q10 dependent tables.
class PostgresQ10TxnLifecycleBackend final : public TxnLifecycleBackend {
public:
	void Install(TxnSession &session) override;
	void Uninstall(TxnSession &session) override;
};

//! Installs PostgreSQL row triggers for SEC EDGAR Q24 (num, pre).
class PostgresQ24TxnLifecycleBackend final : public TxnLifecycleBackend {
public:
	void Install(TxnSession &session) override;
	void Uninstall(TxnSession &session) override;
};

void InstallPostgresQ10Triggers(PostgresAuxSession &session);
void UninstallPostgresQ10Triggers(PostgresAuxSession &session);
void InstallPostgresQ24Triggers(PostgresAuxSession &session);
void UninstallPostgresQ24Triggers(PostgresAuxSession &session);

} // namespace accelerator_codegen::aux_state
