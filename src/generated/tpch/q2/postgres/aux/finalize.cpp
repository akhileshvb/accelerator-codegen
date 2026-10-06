#include "tpch/q2/postgres/aux/module.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/sql_like.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// A qualifying (part, cheapest-regional-supplier) pair, ordered by
// s_acctbal DESC, n_name, s_name, p_partkey.
struct Q2Candidate {
  int64_t acctbal_raw = 0;
  uint32_t nation_rank = 0; // rank of n_name among the matching nations
  int64_t suppkey = 0;
  int64_t partkey = 0;
};

// Everything one Execute derives from its arguments.
struct Q2Query {
  const TpchQ2State *state = nullptr;
  int32_t size = 0;
  std::vector<uint64_t> type_bits; // bit per type-dictionary id: LIKE matches
  // nation key -> rank of its name, for nations of the requested region(s).
  std::vector<int32_t> dense_rank;                 // keys [0, dense_rank.size())
  std::unordered_map<int64_t, int32_t> sparse_rank; // all other keys

  int32_t NationRank(int64_t nationkey) const {
    if (nationkey >= 0 &&
        static_cast<std::size_t>(nationkey) < dense_rank.size())
      return dense_rank[static_cast<std::size_t>(nationkey)];
    const auto it = sparse_rank.find(nationkey);
    return it == sparse_rank.end() ? -1 : it->second;
  }
  bool TypeMatches(uint32_t type_id) const {
    return (type_bits[type_id >> 6] >> (type_id & 63)) & 1;
  }
};

std::string_view SupplierName(const TpchQ2State &state, int64_t suppkey) {
  std::string_view packed;
  state.supplier_text.Get(suppkey, packed);
  return TpchQ2UnpackSupplierText(packed).name;
}

bool Q2Better(const Q2Query &q, const Q2Candidate &a, const Q2Candidate &b) {
  if (a.acctbal_raw != b.acctbal_raw)
    return a.acctbal_raw > b.acctbal_raw;
  if (a.nation_rank != b.nation_rank)
    return a.nation_rank < b.nation_rank;
  const auto an = SupplierName(*q.state, a.suppkey);
  const auto bn = SupplierName(*q.state, b.suppkey);
  if (an != bn)
    return an < bn;
  if (a.partkey != b.partkey)
    return a.partkey < b.partkey;
  return a.suppkey < b.suppkey;
}

using Q2Heap = std::priority_queue<Q2Candidate, std::vector<Q2Candidate>,
                                   std::function<bool(const Q2Candidate &,
                                                      const Q2Candidate &)>>;

// Scan part chunks [chunk_begin, chunk_end): p_size = :size AND p_type LIKE
// :pattern, then the cheapest in-region offers of each qualifying part.
// Considers one part: p_size = :size AND p_type LIKE :pattern, then the
// cheapest in-region offers of the part.
void ConsiderPart(const Q2Query &q, int64_t partkey, Q2Heap &heap) {
  const TpchQ2State &st = *q.state;
  const auto *slots = st.offers.Find(partkey);
  if (!slots || slots->count == 0)
    return;
  // Regional minimum over offers of suppliers in the requested region ...
  int64_t best = std::numeric_limits<int64_t>::max();
  bool any = false;
  for (uint32_t i = 0; i < slots->count; ++i) {
    const auto offer = st.OfferAt(*slots, partkey, i);
    const auto *sup = st.suppliers.Find(offer.suppkey);
    if (!sup || !sup->present || q.NationRank(sup->nationkey) < 0)
      continue;
    any = true;
    best = std::min(best, offer.cost_raw);
  }
  if (!any)
    return;
  // ... and every in-region supplier tied at that minimum is a result row.
  for (uint32_t i = 0; i < slots->count; ++i) {
    const auto offer = st.OfferAt(*slots, partkey, i);
    if (offer.cost_raw != best)
      continue;
    const auto *sup = st.suppliers.Find(offer.suppkey);
    if (!sup || !sup->present)
      continue;
    const int32_t rank = q.NationRank(sup->nationkey);
    if (rank < 0)
      continue;
    if (heap.size() >= kTpchQ2ResultLimit &&
        sup->acctbal_raw < heap.top().acctbal_raw)
      continue; // cannot beat the current 100th row
    heap.push({sup->acctbal_raw, static_cast<uint32_t>(rank), offer.suppkey,
               partkey});
    if (heap.size() > kTpchQ2ResultLimit)
      heap.pop();
  }
}

// Scan part chunks [chunk_begin, chunk_end).
void ScanPartChunks(const Q2Query &q, std::size_t chunk_begin,
                    std::size_t chunk_end, Q2Heap &heap) {
  const TpchQ2State &st = *q.state;
  using SizeCol = core::AnyKeyCowArray<int32_t>;
  for (std::size_t ci = chunk_begin; ci < chunk_end; ++ci) {
    const auto *sizes = st.part_size.ChunkAt(ci);
    const auto *types = st.part_type.ChunkAt(ci);
    if (!sizes || !types)
      continue;
    const int32_t *size_data = sizes->data();
    for (std::size_t slot = 0; slot < SizeCol::kChunkSize; ++slot) {
      if (size_data[slot] != q.size)
        continue;
      const uint32_t type_id = (*types)[slot];
      if (type_id == kTpchQ2NoId || !q.TypeMatches(type_id))
        continue;
      ConsiderPart(q, SizeCol::KeyAt(ci, slot), heap);
    }
  }
}

// Parts whose key is outside the dense range.
void ScanSparseParts(const Q2Query &q, Q2Heap &heap) {
  const TpchQ2State &st = *q.state;
  st.part_size.ForEachSparse([&](int64_t partkey, const int32_t &size) {
    if (size != q.size)
      return;
    const uint32_t type_id = st.part_type.Get(partkey);
    if (type_id == kTpchQ2NoId || !q.TypeMatches(type_id))
      return;
    ConsiderPart(q, partkey, heap);
  });
}

void EmitEmpty(core::AuxRowSink &sink, const core::OutputSchema &schema) {
  sink.Begin(schema);
  sink.End();
}

} // namespace

void TpchQ2FinalizeAndEmit(const TpchQ2State &state, const TpchQ2Args &args,
                           core::AuxRowSink &sink) {
  core::OutputSchema schema;
  schema.columns = {{"s_acctbal", core::AuxType::ScaledDecimal, true},
                    {"s_name", core::AuxType::String, true},
                    {"n_name", core::AuxType::String, true},
                    {"p_partkey", core::AuxType::Int64, true},
                    {"p_mfgr", core::AuxType::String, true},
                    {"s_address", core::AuxType::String, true},
                    {"s_phone", core::AuxType::String, true},
                    {"s_comment", core::AuxType::String, true}};

  // p_size = NULL / p_type LIKE NULL / r_name = NULL are never true; a size
  // outside int32 equals no p_size.
  if (!args.size || !args.type_pattern || !args.region_name ||
      *args.size < std::numeric_limits<int32_t>::min() ||
      *args.size > std::numeric_limits<int32_t>::max()) {
    EmitEmpty(sink, schema);
    return;
  }

  Q2Query q;
  q.state = &state;
  q.size = static_cast<int32_t>(*args.size);

  // Nations of every region named :region, ranked by name (byte order).
  std::vector<int64_t> region_keys;
  for (const auto &region : *state.regions)
    if (region.name == *args.region_name)
      region_keys.push_back(region.key);
  std::vector<const TpchQ2NationRec *> region_nations;
  for (const auto &nation : *state.nations)
    if (std::find(region_keys.begin(), region_keys.end(), nation.regionkey) !=
        region_keys.end())
      region_nations.push_back(&nation);
  if (region_nations.empty()) {
    EmitEmpty(sink, schema);
    return;
  }
  std::vector<std::string_view> names;
  for (const auto *nation : region_nations)
    names.push_back(nation->name);
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  int64_t max_dense = -1;
  for (const auto *nation : region_nations)
    if (nation->key >= 0 && nation->key < 65536)
      max_dense = std::max(max_dense, nation->key);
  q.dense_rank.assign(static_cast<std::size_t>(max_dense + 1), -1);
  for (const auto *nation : region_nations) {
    const int32_t rank = static_cast<int32_t>(
        std::lower_bound(names.begin(), names.end(),
                         std::string_view(nation->name)) -
        names.begin());
    if (nation->key >= 0 && nation->key < 65536)
      q.dense_rank[static_cast<std::size_t>(nation->key)] = rank;
    else
      q.sparse_rank[nation->key] = rank;
  }

  // Evaluate LIKE once per distinct type, not once per part.
  const auto matcher = core::sql_like::LikeMatcher::Compile(*args.type_pattern);
  const auto &blob = state.types->blob();
  q.type_bits = core::sql_like::MatchBlobBitmap(
      matcher, blob.offsets(), state.types->size(), blob.data(),
      state.types->size() > 1000000 ? 4 : 1);
  q.type_bits.push_back(0); // keep TypeMatches in range for an empty dictionary

  const auto better = [&q](const Q2Candidate &a, const Q2Candidate &b) {
    return Q2Better(q, a, b);
  };
  const std::size_t chunks = state.part_size.NumChunks();
  // Large part tables: split the chunk range across threads, merge the heaps.
  const std::size_t threads =
      chunks >= 512 ? std::min<std::size_t>(
                          8, std::max(1u, std::thread::hardware_concurrency()))
                    : 1;
  std::vector<Q2Heap> heaps;
  for (std::size_t t = 0; t < threads; ++t)
    heaps.emplace_back(better);
  ScanSparseParts(q, heaps[0]);
  if (threads == 1) {
    ScanPartChunks(q, 0, chunks, heaps[0]);
  } else {
    std::vector<std::thread> pool;
    const std::size_t per = (chunks + threads - 1) / threads;
    for (std::size_t t = 0; t < threads; ++t)
      pool.emplace_back([&, t] {
        ScanPartChunks(q, t * per, std::min(chunks, (t + 1) * per), heaps[t]);
      });
    for (auto &th : pool)
      th.join();
  }

  std::vector<Q2Candidate> rows;
  for (auto &heap : heaps)
    for (; !heap.empty(); heap.pop())
      rows.push_back(heap.top());
  std::sort(rows.begin(), rows.end(), better);
  if (rows.size() > kTpchQ2ResultLimit)
    rows.resize(kTpchQ2ResultLimit);

  std::unordered_map<int64_t, const std::string *> nation_name;
  for (const auto *nation : region_nations)
    nation_name[nation->key] = &nation->name;

  sink.Begin(schema);
  for (const auto &row : rows) {
    const auto *sup = state.suppliers.Find(row.suppkey);
    std::string_view packed;
    state.supplier_text.Get(row.suppkey, packed);
    const auto text = TpchQ2UnpackSupplierText(packed);
    core::AuxRow out;
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::ScaledDecimal, core::ScaledDecimal{row.acctbal_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::string(text.name)));
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::String, *nation_name.at(sup->nationkey)));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.partkey));
    const uint32_t mfgr_id = state.part_mfgr.Get(row.partkey);
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::String, std::string(state.mfgrs->Get(mfgr_id))));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::string(text.address)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::string(text.phone)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::string(text.comment)));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
