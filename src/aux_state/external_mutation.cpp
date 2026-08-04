#include "accelerator_codegen/aux_state/txn_session.hpp"

namespace accelerator_codegen::aux_state {

void OnExternalMutation(TxnSession &session, const duckdb::TableMutationEvent &event) {
	session.OnTableMutation(event);
}

} // namespace accelerator_codegen::aux_state
