#include "tpch/q5/postgres/aux/module.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "backends/postgres/api/pg_numeric.hpp"

namespace accelerator_codegen::aux_state::query {

void TpchQ5FinalizeAndEmit(std::vector<TpchQ5Group> groups,
                           core::AuxRowSink &sink) {
  sink.Begin({{{"n_name", core::AuxType::String, true},
               {"revenue", core::AuxType::ScaledDecimal, false}}});

  // ORDER BY revenue DESC; ties are unspecified in SQL, so use name ascending
  // (NULL name last) for deterministic output.
  std::sort(groups.begin(), groups.end(),
            [](const TpchQ5Group &a, const TpchQ5Group &b) {
              if (a.revenue != b.revenue)
                return a.revenue > b.revenue;
              if (a.name_is_null != b.name_is_null)
                return b.name_is_null;
              return a.name < b.name;
            });

  for (const auto &group : groups) {
    // Revenue is exact at scale 4; it is emitted as a scaled integer when it
    // fits in 64 bits and as exact decimal text otherwise.
    const bool fits = group.revenue >= INT64_MIN && group.revenue <= INT64_MAX;
    sink.WriteRow(
        {{group.name_is_null
              ? core::AuxValue::Null(core::AuxType::String)
              : core::AuxValue::Of(core::AuxType::String, group.name),
          fits ? core::AuxValue::Of(
                     core::AuxType::ScaledDecimal,
                     core::ScaledDecimal{static_cast<int64_t>(group.revenue), 4})
               : core::AuxValue::Of(
                     core::AuxType::String,
                     pg_numeric::ToText(group.revenue, 4))}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
