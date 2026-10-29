#include "tpch_q16_vgen_verus.h"

#include "substrate/verus/kit/verus_kit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kit = ::accelerator_codegen::verus_kit;

namespace {

const char kQuerySql[] =
    R"SQL(-- TPC-H Q16 (catalog text), :brand as $1, :type_pattern as $2 (NOT LIKE pattern on p_type), :size1..:size8 as $3..$10.
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey AND p_brand <> $1
  AND p_type NOT LIKE $2
  AND p_size IN ($3, $4, $5, $6, $7, $8, $9, $10)
  AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size)SQL";

struct PartColumn {
  std::vector<int64_t> partkey;
  kit::TextColumn brand;
  kit::TextColumn type;
  std::vector<int64_t> size;

  explicit PartColumn(std::size_t reserve = 0) {
    partkey.reserve(reserve);
    size.reserve(reserve);
    brand.bytes.reserve(reserve * 8);
    brand.off.reserve(reserve + 1);
    type.bytes.reserve(reserve * 16);
    type.off.reserve(reserve + 1);
  }

  void Push(const kit::core::AuxRow &row) {
    partkey.push_back(kit::I64(row, 0, 0));
    brand.Push(kit::Text(row, 1));
    type.Push(kit::Text(row, 2));
    size.push_back(kit::I64(row, 3, 0));
  }
};

struct EdgeColumn {
  std::vector<tpchq16vgen_edge_input> rows;

  explicit EdgeColumn(std::size_t reserve = 0) { rows.reserve(reserve); }

  void Push(const kit::core::AuxRow &row) {
    rows.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0)});
  }
};

struct BlockerColumn {
  std::vector<tpchq16vgen_blocker_input> rows;

  explicit BlockerColumn(std::size_t reserve = 0) { rows.reserve(reserve); }

  void Push(const kit::core::AuxRow &row) {
    rows.push_back({kit::I64(row, 0, 0)});
  }
};

struct PartDelta {
  std::vector<int64_t> partkey;
  kit::TextColumn brand;
  kit::TextColumn type;
  std::vector<int64_t> size;
  std::vector<int64_t> sign;

  explicit PartDelta(std::size_t reserve = 0) {
    partkey.reserve(reserve);
    size.reserve(reserve);
    sign.reserve(reserve);
    brand.bytes.reserve(reserve * 8);
    brand.off.reserve(reserve + 1);
    type.bytes.reserve(reserve * 16);
    type.off.reserve(reserve + 1);
  }

  void Push(const kit::Change &change) {
    const kit::core::AuxRow &row = change.row;
    partkey.push_back(kit::I64(row, 0, 0));
    brand.Push(kit::Text(row, 1));
    type.Push(kit::Text(row, 2));
    size.push_back(kit::I64(row, 3, 0));
    sign.push_back(change.sign);
  }
};

struct EdgeDelta {
  std::vector<tpchq16vgen_edge_delta> rows;

  explicit EdgeDelta(std::size_t reserve = 0) { rows.reserve(reserve); }

  void Push(const kit::Change &change) {
    const kit::core::AuxRow &row = change.row;
    rows.push_back({{kit::I64(row, 0, 0), kit::I64(row, 1, 0)}, change.sign});
  }
};

struct SupplierDelta {
  std::vector<tpchq16vgen_supplier_delta> rows;

  explicit SupplierDelta(std::size_t reserve = 0) { rows.reserve(reserve); }

  void Push(const kit::Change &change) {
    const kit::core::AuxRow &row = change.row;
    const std::string &comment = kit::Text(row, 1);
    const std::size_t customer = comment.find("Customer");
    const std::size_t complaints =
        customer == std::string::npos
            ? std::string::npos
            : comment.find("Complaints", customer + 8);
    const uint8_t is_blocker = complaints == std::string::npos ? 0 : 1;
    rows.push_back({kit::I64(row, 0, 0), is_blocker, change.sign});
  }
};

struct Deltas {
  PartDelta part;
  EdgeDelta edge;
  SupplierDelta supplier;
};

void MarshalDeltas(const kit::Changes &changes, Deltas &deltas) {
  deltas.part = PartDelta(changes[0].size());
  deltas.edge = EdgeDelta(changes[1].size());
  deltas.supplier = SupplierDelta(changes[2].size());
  for (const kit::Change &change : changes[0]) {
    deltas.part.Push(change);
  }
  for (const kit::Change &change : changes[1]) {
    deltas.edge.Push(change);
  }
  for (const kit::Change &change : changes[2]) {
    deltas.supplier.Push(change);
  }
}

struct RescanRows {
  std::vector<int64_t> edge_keys;
  EdgeColumn edge_rows;
  std::vector<int64_t> edge_supplier_keys;
  EdgeColumn edge_supplier_rows;
  std::vector<int64_t> part_keys;
  PartColumn part_rows;
  std::vector<int64_t> blocker_keys;
  BlockerColumn blocker_rows;
};

void CollectRescanKeys(const tpchq16vgen_state *state,
                       const Deltas &deltas,
                       size_t (*collect)(const tpchq16vgen_state *, size_t,
                                         const int64_t *, const uint8_t *,
                                         const size_t *, const uint8_t *,
                                         const size_t *, const int64_t *,
                                         const int64_t *, size_t,
                                         const tpchq16vgen_edge_delta *, size_t,
                                         const tpchq16vgen_supplier_delta *,
                                         int64_t *, size_t),
                       std::vector<int64_t> &keys) {
  size_t cap = 64;
  keys.resize(cap);
  for (;;) {
    const size_t count =
        collect(state, deltas.part.partkey.size(), deltas.part.partkey.data(),
                deltas.part.brand.bytes.data(), deltas.part.brand.off.data(),
                deltas.part.type.bytes.data(), deltas.part.type.off.data(),
                deltas.part.size.data(), deltas.part.sign.data(),
                deltas.edge.rows.size(), deltas.edge.rows.data(),
                deltas.supplier.rows.size(), deltas.supplier.rows.data(),
                keys.data(), cap);
    if (count <= cap) {
      keys.resize(count);
      return;
    }
    cap = count;
    keys.resize(cap);
  }
}

void QueryEdgeRows(kit::Host &host, const char *key_column,
                   const std::vector<int64_t> &keys, EdgeColumn &rows) {
  kit::QueryKeys(host, "SELECT ps_partkey, ps_suppkey FROM partsupp",
                 key_column, keys,
                 [&rows](const kit::core::AuxRow &row) { rows.Push(row); });
}

void QueryPartRows(kit::Host &host, const std::vector<int64_t> &keys,
                   PartColumn &rows) {
  kit::QueryKeys(
      host,
      "SELECT p_partkey, p_brand, p_type, p_size FROM part", "p_partkey", keys,
      [&rows](const kit::core::AuxRow &row) { rows.Push(row); });
}

void QueryBlockerRows(kit::Host &host, const std::vector<int64_t> &keys,
                      BlockerColumn &rows) {
  kit::QueryKeys(
      host,
      "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%'",
      "s_suppkey", keys,
      [&rows](const kit::core::AuxRow &row) { rows.Push(row); });
}

void SortUnique(std::vector<int64_t> &keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

// The kernel's key functions see only the deltas, but the rows it needs depend
// on earlier rescans: the suppliers of a changed part are known only from the
// partsupp rows now in the database, and the parts to load are those of the
// affected suppliers' current partsupp rows.  The keys it is handed may be any
// superset of what it asks for (rows are filtered by its own scopes), so the
// glue chains the rescans: edges of changed parts -> their suppliers join the
// supplier keys -> edges and blockers of those suppliers -> their parts join the
// neighbor keys.
void CollectRescans(const tpchq16vgen_state *state, const Deltas &deltas,
                    kit::Host &host, RescanRows &rows) {
  CollectRescanKeys(
      state, deltas, tpchq16vgen_rescan_edges_for_parts_keys, rows.edge_keys);
  CollectRescanKeys(state, deltas, tpchq16vgen_rescan_edges_for_suppliers_keys,
                    rows.edge_supplier_keys);
  CollectRescanKeys(state, deltas, tpchq16vgen_rescan_parts_for_neighbors_keys,
                    rows.part_keys);
  CollectRescanKeys(state, deltas,
                    tpchq16vgen_rescan_blockers_for_suppliers_keys,
                    rows.blocker_keys);
  QueryEdgeRows(host, "ps_partkey", rows.edge_keys, rows.edge_rows);
  for (const auto &edge : rows.edge_rows.rows) {
    rows.edge_supplier_keys.push_back(edge.ps_suppkey);
    rows.blocker_keys.push_back(edge.ps_suppkey);
  }
  SortUnique(rows.edge_supplier_keys);
  SortUnique(rows.blocker_keys);
  QueryEdgeRows(host, "ps_suppkey", rows.edge_supplier_keys,
                rows.edge_supplier_rows);
  for (const auto &edge : rows.edge_supplier_rows.rows)
    rows.part_keys.push_back(edge.ps_partkey);
  SortUnique(rows.part_keys);
  QueryPartRows(host, rows.part_keys, rows.part_rows);
  QueryBlockerRows(host, rows.blocker_keys, rows.blocker_rows);
}

void LoadBuild(kit::Host &host, PartColumn &parts, EdgeColumn &edges,
               BlockerColumn &blockers) {
  host.Query(
      "SELECT p_partkey, p_brand, p_type, p_size FROM part",
      [&parts](const kit::core::AuxRow &row) { parts.Push(row); });
  host.Query("SELECT ps_partkey, ps_suppkey FROM partsupp",
             [&edges](const kit::core::AuxRow &row) { edges.Push(row); });
  host.Query("SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%'",
             [&blockers](const kit::core::AuxRow &row) { blockers.Push(row); });
}

void *Build(kit::Host &host) {
  PartColumn parts;
  EdgeColumn edges;
  BlockerColumn blockers;
  LoadBuild(host, parts, edges, blockers);
  return tpchq16vgen_build(
      parts.partkey.size(), parts.partkey.data(), parts.brand.bytes.data(),
      parts.brand.off.data(), parts.type.bytes.data(), parts.type.off.data(),
      parts.size.data(), edges.rows.size(), edges.rows.data(),
      blockers.rows.size(), blockers.rows.data());
}

void FreeState(void *state) {
  tpchq16vgen_free(static_cast<tpchq16vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq16vgen_state *>(state);
  Deltas deltas;
  MarshalDeltas(changes, deltas);
  RescanRows rows;
  CollectRescans(kernel_state, deltas, host, rows);
  return tpchq16vgen_apply(
      kernel_state, deltas.part.partkey.size(), deltas.part.partkey.data(),
      deltas.part.brand.bytes.data(), deltas.part.brand.off.data(),
      deltas.part.type.bytes.data(), deltas.part.type.off.data(),
      deltas.part.size.data(), deltas.part.sign.data(), deltas.edge.rows.size(),
      deltas.edge.rows.data(), deltas.supplier.rows.size(),
      deltas.supplier.rows.data(), rows.edge_keys.size(), rows.edge_keys.data(),
      rows.edge_rows.rows.size(), rows.edge_rows.rows.data(),
      rows.edge_supplier_keys.size(), rows.edge_supplier_keys.data(),
      rows.edge_supplier_rows.rows.size(), rows.edge_supplier_rows.rows.data(),
      rows.part_keys.size(), rows.part_keys.data(), rows.part_rows.partkey.size(),
      rows.part_rows.partkey.data(), rows.part_rows.brand.bytes.data(),
      rows.part_rows.brand.off.data(), rows.part_rows.type.bytes.data(),
      rows.part_rows.type.off.data(), rows.part_rows.size.data(),
      rows.blocker_keys.size(), rows.blocker_keys.data(),
      rows.blocker_rows.rows.size(), rows.blocker_rows.rows.data(), err);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string brand;
  std::string pattern;
  int64_t sizes[8] = {};
  if (!kit::ArgText(args, 0, brand) || !kit::ArgText(args, 1, pattern)) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if (!kit::ArgI64(args, 2 + i, 0, sizes[i])) {
      return false;
    }
  }

  tpchq16vgen_result *result = call(reinterpret_cast<const uint8_t *>(brand.data()), brand.size(),
      reinterpret_cast<const uint8_t *>(pattern.data()), pattern.size(),
      sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], sizes[5], sizes[6],
      sizes[7]);
  if (result == nullptr) {
    return false;
  }

  const size_t count = tpchq16vgen_result_len(result);
  out.clear();
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    kit::core::AuxRow row;
    row.values.reserve(4);
    const uint8_t *text = nullptr;
    size_t length = 0;
    if (tpchq16vgen_result_c0(result, i, &text, &length)) {
      row.values.push_back(kit::OutText(text, length));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::String));
    }
    if (tpchq16vgen_result_c1(result, i, &text, &length)) {
      row.values.push_back(kit::OutText(text, length));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::String));
    }
    int64_t value = 0;
    if (tpchq16vgen_result_c2(result, i, &value)) {
      row.values.push_back(kit::OutI64(value));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::Int64));
    }
    if (tpchq16vgen_result_c3(result, i, &value)) {
      row.values.push_back(kit::OutI64(value));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::Int64));
    }
    out.push_back(std::move(row));
  }
  tpchq16vgen_result_free(result);
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq16vgen_execute(static_cast<const tpchq16vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq16vgens_answer(static_cast<const tpchq16vgens_reading *>(state), x...);
  });
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host,
                   int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq16vgens_base *>(base);
  const tpchq16vgen_state *state = tpchq16vgens_base_state(kernel_base);
  Deltas deltas;
  MarshalDeltas(changes, deltas);
  RescanRows rows;
  CollectRescans(state, deltas, host, rows);
  return tpchq16vgens_stage_apply(
      kernel_base, deltas.part.partkey.size(),
      deltas.part.partkey.data(), deltas.part.brand.bytes.data(),
      deltas.part.brand.off.data(), deltas.part.type.bytes.data(),
      deltas.part.type.off.data(), deltas.part.size.data(),
      deltas.part.sign.data(), deltas.edge.rows.size(), deltas.edge.rows.data(),
      deltas.supplier.rows.size(), deltas.supplier.rows.data(),
      rows.edge_keys.size(), rows.edge_keys.data(), rows.edge_rows.rows.size(),
      rows.edge_rows.rows.data(), rows.edge_supplier_keys.size(),
      rows.edge_supplier_keys.data(), rows.edge_supplier_rows.rows.size(),
      rows.edge_supplier_rows.rows.data(), rows.part_keys.size(),
      rows.part_keys.data(), rows.part_rows.partkey.size(),
      rows.part_rows.partkey.data(), rows.part_rows.brand.bytes.data(),
      rows.part_rows.brand.off.data(), rows.part_rows.type.bytes.data(),
      rows.part_rows.type.off.data(), rows.part_rows.size.data(),
      rows.blocker_keys.size(), rows.blocker_keys.data(),
      rows.blocker_rows.rows.size(), rows.blocker_rows.rows.data(), err);
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  PartColumn parts;
  EdgeColumn edges;
  BlockerColumn blockers;
  LoadBuild(host, parts, edges, blockers);
  return tpchq16vgens_stage_build(
      static_cast<const tpchq16vgens_head *>(head_of()), parts.partkey.size(),
      parts.partkey.data(), parts.brand.bytes.data(), parts.brand.off.data(),
      parts.type.bytes.data(), parts.type.off.data(), parts.size.data(),
      edges.rows.size(), edges.rows.data(), blockers.rows.size(),
      blockers.rows.data());
}

} // namespace



const kit::Glue kGlue = {
    "tpch_q16_vgen",
    "tpch",
    16,
    kQuerySql,
    {{"part", {"p_partkey", "p_brand", "p_type", "p_size"}},
     {"partsupp", {"ps_partkey", "ps_suppkey"}},
     {"supplier", {"s_suppkey", "s_comment"}}},
    {kit::Kind::Text, kit::Kind::Text, kit::Kind::Int, kit::Kind::Int,
     kit::Kind::Int, kit::Kind::Int, kit::Kind::Int, kit::Kind::Int,
     kit::Kind::Int, kit::Kind::Int},
    Build,
    FreeState,
    Apply,
    Execute,
    Answer,
    PrepareApply,
    PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq16vgens_new(); },
        [](void *store) { tpchq16vgens_free(static_cast<tpchq16vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq16vgens_newest_base(static_cast<const tpchq16vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq16vgens_base_state(static_cast<const tpchq16vgens_base *>(base)); },
        [](void *base) { tpchq16vgens_base_free(static_cast<tpchq16vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq16vgens_newest_head(static_cast<const tpchq16vgens_store *>(store)); },
        [](void *head) { tpchq16vgens_head_free(static_cast<tpchq16vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq16vgens_stage_invalid(static_cast<const tpchq16vgens_head *>(head)); },
        [](void *staged) { tpchq16vgens_staged_free(static_cast<tpchq16vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq16vgens_begin(static_cast<const tpchq16vgens_store *>(store), from); },
        [](const void *store) { tpchq16vgens_abort(static_cast<const tpchq16vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq16vgens_install(static_cast<const tpchq16vgens_store *>(store), static_cast<tpchq16vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq16vgens_read(static_cast<const tpchq16vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq16vgens_reading_free(const_cast<tpchq16vgens_reading *>(static_cast<const tpchq16vgens_reading *>(reading))); },
        [](const void *store) { tpchq16vgens_invalidate_all(static_cast<const tpchq16vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq16vgens_version_count(static_cast<const tpchq16vgens_store *>(store)); },
        []() -> std::size_t { return tpchq16vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq16vgens_heap_peak_bytes(); },
        []() { tpchq16vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq16vgens_stage_adopt(static_cast<const tpchq16vgens_head *>(head), static_cast<tpchq16vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);
