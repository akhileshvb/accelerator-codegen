#include "tpch/q16/mysql/aux/module.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/sql_like.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

struct Q16Row {
  uint32_t brand_id = 0;
  uint32_t type_id = 0;
  int32_t size = 0;
  int64_t count = 0;
};

} // namespace

void TpchQ16FinalizeAndEmit(const TpchQ16State &state, const TpchQ16Args &args,
                            core::AuxRowSink &sink) {
  sink.Begin({{{"p_brand", core::AuxType::String, false},
               {"p_type", core::AuxType::String, false},
               {"p_size", core::AuxType::Int64, false},
               {"supplier_cnt", core::AuxType::Int64, false}}});
  // p_brand <> NULL and p_type NOT LIKE NULL are never true.
  if (!args.brand || !args.type_pattern) {
    sink.End();
    return;
  }

  // The IN-list: NULL items already dropped; duplicates and out-of-int32
  // values (equal to no p_size) are irrelevant.
  std::vector<int32_t> sizes;
  for (const int64_t size : args.sizes)
    if (size >= std::numeric_limits<int32_t>::min() &&
        size <= std::numeric_limits<int32_t>::max())
      sizes.push_back(static_cast<int32_t>(size));
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

  // p_type NOT LIKE :pattern, evaluated once per distinct type (a bitmap of
  // the types that MATCH, which the NOT LIKE rejects).
  const auto matcher = core::sql_like::LikeMatcher::Compile(*args.type_pattern);
  const auto &blob = state.types->blob();
  const auto like_bits = core::sql_like::MatchBlobBitmap(
      matcher, blob.offsets(), state.types->size(), blob.data());
  // p_brand <> :brand: at most one brand id is excluded.
  const uint32_t excluded_brand = state.brands->Find(*args.brand);

  std::vector<Q16Row> rows;
  const auto &table = *state.group_table;
  for (const int32_t size : sizes) {
    const auto it = table.by_size.find(size);
    if (it == table.by_size.end())
      continue;
    for (const uint32_t gid : it->second) {
      const auto &key = table.keys[gid];
      if (key.brand_id == excluded_brand)
        continue;
      if ((like_bits[key.type_id >> 6] >> (key.type_id & 63)) & 1)
        continue;
      const int64_t count = state.distinct.Get(gid);
      if (count > 0)
        rows.push_back({key.brand_id, key.type_id, size, count});
    }
  }

  // ORDER BY supplier_cnt DESC, p_brand, p_type, p_size (byte-wise text order).
  std::sort(rows.begin(), rows.end(), [&](const Q16Row &a, const Q16Row &b) {
    if (a.count != b.count)
      return a.count > b.count;
    if (a.brand_id != b.brand_id) {
      const auto ba = state.brands->Get(a.brand_id);
      const auto bb = state.brands->Get(b.brand_id);
      if (ba != bb)
        return ba < bb;
    }
    if (a.type_id != b.type_id) {
      const auto ta = state.types->Get(a.type_id);
      const auto tb = state.types->Get(b.type_id);
      if (ta != tb)
        return ta < tb;
    }
    return a.size < b.size;
  });

  core::AuxRow row;
  row.values.resize(4);
  for (const auto &r : rows) {
    row.values[0] = core::AuxValue::Of(
        core::AuxType::String, std::string(state.brands->Get(r.brand_id)));
    row.values[1] = core::AuxValue::Of(
        core::AuxType::String, std::string(state.types->Get(r.type_id)));
    row.values[2] =
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(r.size));
    row.values[3] = core::AuxValue::Of(core::AuxType::Int64, r.count);
    sink.WriteRow(row);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
