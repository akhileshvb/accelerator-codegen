#include "tpch/q12/duckdb/aux/module.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {

int TpchQ12ModeSlot(const std::string &shipmode) {
  const char *c = shipmode.data();
  switch (shipmode.size()) {
  case 3:
    if (std::memcmp(c, "AIR", 3) == 0)
      return 0;
    if (std::memcmp(c, "FOB", 3) == 0)
      return 1;
    return -1;
  case 4:
    if (std::memcmp(c, "MAIL", 4) == 0)
      return 2;
    if (std::memcmp(c, "RAIL", 4) == 0)
      return 3;
    if (std::memcmp(c, "SHIP", 4) == 0)
      return 4;
    return -1;
  case 5:
    return std::memcmp(c, "TRUCK", 5) == 0 ? 5 : -1;
  case 7:
    return std::memcmp(c, "REG AIR", 7) == 0 ? 6 : -1;
  default:
    return -1;
  }
}

void TpchQ12Modes::Add(int slot, const std::string &shipmode, int64_t h,
                       int64_t l) {
  if (slot >= 0) {
    high[slot] += h;
    low[slot] += l;
    return;
  }
  for (auto it = extra.begin(); it != extra.end(); ++it) {
    if (it->shipmode == shipmode) {
      it->high_line_count += h;
      it->low_line_count += l;
      if (it->high_line_count == 0 && it->low_line_count == 0)
        extra.erase(it);
      return;
    }
  }
  if (h != 0 || l != 0)
    extra.push_back({shipmode, h, l});
}

bool TpchQ12Modes::Empty() const {
  int64_t any = 0;
  for (int i = 0; i < kQ12ModeSlots; ++i)
    any |= high[i] | low[i];
  return any == 0 && extra.empty();
}

namespace {

struct ModeTotals {
  int64_t high = 0;
  int64_t low = 0;
};

} // namespace

void TpchQ12FinalizeAndEmit(const TpchQ12Modes *const *parts,
                            std::size_t part_count, const std::string &shipmode1,
                            const std::string &shipmode2,
                            core::AuxRowSink &sink) {
  // Branch-free dense accumulation over all slots (two 512-bit adds per node),
  // then pick the requested modes.
  alignas(64) int64_t high[kQ12ModeSlots] = {};
  alignas(64) int64_t low[kQ12ModeSlots] = {};
  bool any_extra = false;
  for (std::size_t p = 0; p < part_count; ++p) {
    const auto *part = parts[p];
    if (!part)
      continue;
    for (int i = 0; i < kQ12ModeSlots; ++i) {
      high[i] += part->high[i];
      low[i] += part->low[i];
    }
    any_extra |= !part->extra.empty();
  }
  auto totals_of = [&](const std::string &mode) {
    ModeTotals t;
    const int slot = TpchQ12ModeSlot(mode);
    if (slot >= 0) {
      t.high = high[slot];
      t.low = low[slot];
    } else if (any_extra) {
      for (std::size_t p = 0; p < part_count; ++p) {
        if (!parts[p])
          continue;
        for (const auto &entry : parts[p]->extra) {
          if (entry.shipmode == mode) {
            t.high += entry.high_line_count;
            t.low += entry.low_line_count;
          }
        }
      }
    }
    return t;
  };
  const bool modes_equal = shipmode1 == shipmode2;
  const ModeTotals s1 = totals_of(shipmode1);
  const ModeTotals s2 = modes_equal ? ModeTotals{} : totals_of(shipmode2);
  const bool emit1 = s1.high != 0 || s1.low != 0;
  const bool emit2 = !modes_equal && (s2.high != 0 || s2.low != 0);
  static const core::OutputSchema kSchema{
      {{"l_shipmode", core::AuxType::String, false},
       {"high_line_count", core::AuxType::Int64, false},
       {"low_line_count", core::AuxType::Int64, false}}};
  sink.Begin(kSchema);
  core::AuxRow row;
  row.values.resize(3);
  row.values[0].type = core::AuxType::String;
  row.values[1].type = core::AuxType::Int64;
  row.values[2].type = core::AuxType::Int64;
  for (auto &value : row.values)
    value.is_null = false;
  auto write = [&](const std::string &mode, const ModeTotals &t) {
    row.values[0].payload = mode;
    row.values[1].payload = static_cast<int64_t>(t.high);
    row.values[2].payload = static_cast<int64_t>(t.low);
    sink.WriteRow(row);
  };
  if (emit1 && emit2) {
    if (shipmode2 < shipmode1) {
      write(shipmode2, s2);
      write(shipmode1, s1);
    } else {
      write(shipmode1, s1);
      write(shipmode2, s2);
    }
  } else if (emit1) {
    write(shipmode1, s1);
  } else if (emit2) {
    write(shipmode2, s2);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
