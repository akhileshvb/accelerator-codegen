#include "tpch/q12/mysql/aux/module.hpp"
#include "accelerator_codegen/stateful/core/mysql_collation.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {

void TpchQ12FinalizeAndEmit(std::vector<TpchQ12Group> groups,
                            core::AuxRowSink &sink) {
  std::sort(groups.begin(), groups.end(),
            [](const TpchQ12Group &a, const TpchQ12Group &b) {
              return mysql_collation::LessCi(a.shipmode, b.shipmode);
            });
  sink.Begin({{{"l_shipmode", core::AuxType::String, false},
               {"high_line_count", core::AuxType::Int64, false},
               {"low_line_count", core::AuxType::Int64, false}}});
  for (const auto &group : groups)
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::String, group.shipmode),
          core::AuxValue::Of(core::AuxType::Int64, group.counts.high),
          core::AuxValue::Of(core::AuxType::Int64, group.counts.low)}});
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
