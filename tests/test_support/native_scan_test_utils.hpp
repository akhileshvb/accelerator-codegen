#pragma once

#include <cstdint>
#include <vector>

#include "duckdb/common/types/date.hpp"

namespace duckdb {
class ClientContext;
class Connection;
}

namespace accelerator_codegen::test_support {

uint64_t CountLineitemRows(duckdb::Connection &connection);
uint64_t CountLineitemRows(duckdb::ClientContext &context);
std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::Connection &connection);
std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::ClientContext &context);

} // namespace accelerator_codegen::test_support
