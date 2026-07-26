#pragma once

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state {

void InstallAcceleratorSubstrate(duckdb::Connection &connection);

} // namespace accelerator_codegen::aux_state
