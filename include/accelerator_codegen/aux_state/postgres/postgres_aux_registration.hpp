#pragma once

#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"

namespace accelerator_codegen {

void RegisterQ10AuxAcceleratorPostgres(aux_state::PostgresAuxSession &session);
void RegisterSecEdgarQ24AuxAcceleratorPostgres(aux_state::PostgresAuxSession &session);

} // namespace accelerator_codegen
