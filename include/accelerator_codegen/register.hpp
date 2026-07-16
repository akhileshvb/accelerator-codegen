#pragma once

namespace duckdb {
class Connection;
}

namespace accelerator_codegen {

void RegisterAcceleratorFunctions(duckdb::Connection &connection);

} // namespace accelerator_codegen
