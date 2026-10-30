// The server-side entry points of pg_base, for test clients that link libaccelerator_codegen (the query
// catalog, the registrars of the per-query code).  The module defines them for real; a client never
// reaches a scan.
#include <stdexcept>

#include "backends/postgres/api/base/pg_base.hpp"

namespace accelerator_codegen::pg_base {

void ScanTable(const ScanSpec &, const std::function<void(const Batch &)> &) {
  throw std::logic_error("pg_base::ScanTable runs in the server");
}

std::vector<uint8_t> KeysExist(const std::string &, const std::string &, const std::vector<int64_t> &) {
  throw std::logic_error("pg_base::KeysExist runs in the server");
}

} // namespace accelerator_codegen::pg_base
