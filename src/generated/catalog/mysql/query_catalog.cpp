// MySQL query catalog registry.  Names no query: entries are registered by the
// per-query catalog.cpp files (see query_catalog.hpp).
#include "catalog/mysql/query_catalog.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

// Function-local statics (no header-only singleton); a deque keeps entries
// address-stable, and lookups work on a view sorted by query id.
std::deque<MysqlSharedQueryCatalogEntry> &Entries() {
  static std::deque<MysqlSharedQueryCatalogEntry> entries;
  return entries;
}

std::mutex &EntriesMutex() {
  static std::mutex mu;
  return mu;
}

bool SameKey(const MysqlSharedQueryCatalogEntry &a, const MysqlSharedQueryCatalogEntry &b) {
  return a.query_id == b.query_id && std::string(a.workload_name) == b.workload_name;
}

const MysqlSharedQueryCatalogEntry *FindEntry(const std::string &workload_name, int query_id) {
  std::lock_guard<std::mutex> lock(EntriesMutex());
  for (const auto &e : Entries()) {
    if (e.query_id == query_id && workload_name == e.workload_name) {
      return &e;
    }
  }
  return nullptr;
}

} // namespace

void RegisterMysqlQuery(const MysqlSharedQueryCatalogEntry &entry) {
  std::lock_guard<std::mutex> lock(EntriesMutex());
  for (auto &e : Entries()) {
    if (SameKey(e, entry)) {
      e = entry;
      return;
    }
  }
  Entries().push_back(entry);
}

const MysqlSharedQueryCatalogEntry &LookupMysqlQuery(const std::string &workload_name,
                                                     int query_id) {
  const auto *entry = FindEntry(workload_name, query_id);
  if (!entry) {
    throw std::runtime_error(
        "mysql query not found: " + workload_name + " Q" + std::to_string(query_id));
  }
  return *entry;
}

std::vector<int> MysqlQueriesForWorkload(const std::string &workload_name) {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(EntriesMutex());
    for (const auto &e : Entries()) {
      if (workload_name == e.workload_name) ids.push_back(e.query_id);
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace accelerator_codegen::aux_state::engine::mysql
