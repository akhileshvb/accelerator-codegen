// Conformance: structural Dafny twins vs real postgres Q1 finalize.cpp + module.cpp.
// One side is always the generated artifact. Model* functions are literal twins of
// tests/verification/q1/dafny/Q1Finalize.dfy and Q1ModuleTwin.dfy (same formulas).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/state/invalidation_plan.hpp"
#include "tpch/q1/postgres/aux/finalize.hpp"
#include "tpch/q1/postgres/aux/module.hpp"

using accelerator_codegen::ApplyQ1RowContribution;
using accelerator_codegen::FinalizeQ1MergedGroups;
using accelerator_codegen::MergeQ1DayGroupStats;
using accelerator_codegen::PackQ1GroupKey;
using accelerator_codegen::Q1AccT;
using accelerator_codegen::Q1DayGroupStats;
using accelerator_codegen::Q1DayPartition;
using accelerator_codegen::Q1GroupKey;
using accelerator_codegen::Q1MergedGroupStats;
using accelerator_codegen::Q1OutputRow;
using accelerator_codegen::UnpackQ1Linestatus;
using accelerator_codegen::UnpackQ1Returnflag;
using accelerator_codegen::aux_state::core::AuxDate;
using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::MutationBatch;
using accelerator_codegen::aux_state::core::MutationKind;
using accelerator_codegen::aux_state::core::ScaledDecimal;
using accelerator_codegen::aux_state::query::DecodeTpchQ1Day;
using accelerator_codegen::aux_state::query::EncodeTpchQ1Day;
using accelerator_codegen::aux_state::query::TpchQ1Config;
using accelerator_codegen::aux_state::query::TpchQ1CutoffDays;
using accelerator_codegen::aux_state::query::TpchQ1Module;
using accelerator_codegen::aux_state::query::TpchQ1TxnBuffer;

namespace {

// ----- Q1Finalize twin (Apply / Merge / HugeintScaleToDouble) -----

struct ModelStats {
  int64_t count = 0;
  int64_t sum_qty = 0;
  int64_t sum_base_price = 0;
  Q1AccT sum_disc_price = 0;
  Q1AccT sum_charge = 0;
  Q1AccT sum_discount = 0;
};

ModelStats ModelApply(ModelStats s, int64_t qty, int64_t price, int64_t disc,
                      int64_t tax, int8_t sign) {
  // Twin of ApplyQ1RowContribution / Q1Finalize.ApplyQ1RowContribution
  const auto one_minus_discount_raw = static_cast<int64_t>(100 - disc);
  const Q1AccT disc_price_raw =
      static_cast<Q1AccT>(price) * static_cast<Q1AccT>(one_minus_discount_raw);
  s.count += sign;
  s.sum_qty += sign * qty;
  s.sum_base_price += sign * price;
  s.sum_discount += sign * disc;
  s.sum_disc_price += static_cast<Q1AccT>(sign) * disc_price_raw;
  const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax);
  s.sum_charge +=
      static_cast<Q1AccT>(sign) * disc_price_raw * static_cast<Q1AccT>(one_plus_tax_raw);
  return s;
}

ModelStats ModelMerge(ModelStats a, const Q1DayGroupStats &b) {
  a.count += b.count;
  a.sum_qty += b.sum_qty;
  a.sum_base_price += b.sum_base_price;
  a.sum_disc_price += b.sum_disc_price;
  a.sum_charge += b.sum_charge;
  a.sum_discount += b.sum_discount;
  return a;
}

// Twin of HugeintScaleToDouble (finalize.cpp) — same divide-by-10 loop.
double ModelHugeintScaleToDouble(Q1AccT value, uint8_t scale, uint64_t count) {
  if (count == 0)
    return 0.0;
  double scaled = static_cast<double>(value);
  while (scale > 0) {
    scaled /= 10.0;
    scale--;
  }
  return scaled / static_cast<double>(count);
}

bool EqualDay(const Q1DayGroupStats &c, const ModelStats &m) {
  return c.count == m.count && c.sum_qty == m.sum_qty &&
         c.sum_base_price == m.sum_base_price &&
         c.sum_disc_price == m.sum_disc_price && c.sum_charge == m.sum_charge &&
         c.sum_discount == m.sum_discount;
}

bool EqualMerged(const Q1MergedGroupStats &c, const ModelStats &m) {
  return c.count == m.count && c.sum_qty == m.sum_qty &&
         c.sum_base_price == m.sum_base_price &&
         c.sum_disc_price == m.sum_disc_price && c.sum_charge == m.sum_charge &&
         c.sum_discount == m.sum_discount;
}

bool GroupEqual(const Q1DayGroupStats &a, const Q1DayGroupStats &b) {
  return a.count == b.count && a.sum_qty == b.sum_qty &&
         a.sum_base_price == b.sum_base_price &&
         a.sum_disc_price == b.sum_disc_price && a.sum_charge == b.sum_charge &&
         a.sum_discount == b.sum_discount;
}

bool DoubleBitsEqual(double a, double b) {
  uint64_t ua = 0, ub = 0;
  static_assert(sizeof(double) == sizeof(uint64_t));
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

int64_t HugeintAsI64(const duckdb::hugeint_t &v) {
  assert(v.upper == 0 || v.upper == -1);
  return static_cast<int64_t>(v.lower);
}

// Twin of ScaleToRaw (module.cpp:~143) — anonymous; bound by line-faithful copy.
int64_t ModelScaleToRaw(int64_t raw, int32_t scale, int32_t target_scale) {
  if (scale == target_scale)
    return raw;
  if (scale < target_scale) {
    int64_t factor = 1;
    for (int32_t i = scale; i < target_scale; ++i)
      factor *= 10;
    return raw * factor;
  }
  int64_t factor = 1;
  for (int32_t i = target_scale; i < scale; ++i)
    factor *= 10;
  return raw / factor;
}

void CheckApply(int64_t qty, int64_t price, int64_t disc, int64_t tax,
                int8_t sign) {
  Q1DayGroupStats cpp{};
  ApplyQ1RowContribution(cpp, qty, price, disc, tax, sign);
  ModelStats model = ModelApply({}, qty, price, disc, tax, sign);
  assert(EqualDay(cpp, model));
}

void CheckApplyThenCancel(int64_t qty, int64_t price, int64_t disc,
                          int64_t tax) {
  Q1DayGroupStats cpp{};
  ApplyQ1RowContribution(cpp, qty, price, disc, tax, 1);
  ApplyQ1RowContribution(cpp, qty, price, disc, tax, -1);
  assert(cpp.count == 0);
  assert(cpp.sum_qty == 0);
  assert(cpp.sum_base_price == 0);
  assert(cpp.sum_disc_price == 0);
  assert(cpp.sum_charge == 0);
  assert(cpp.sum_discount == 0);
}

void CheckMerge(int64_t qty1, int64_t price1, int64_t disc1, int64_t tax1,
                int64_t qty2, int64_t price2, int64_t disc2, int64_t tax2) {
  Q1DayGroupStats d1{};
  Q1DayGroupStats d2{};
  ApplyQ1RowContribution(d1, qty1, price1, disc1, tax1, 1);
  ApplyQ1RowContribution(d2, qty2, price2, disc2, tax2, 1);

  Q1MergedGroupStats merged{};
  MergeQ1DayGroupStats(merged, d1);
  MergeQ1DayGroupStats(merged, d2);

  ModelStats model{};
  model = ModelMerge(model, d1);
  model = ModelMerge(model, d2);
  assert(EqualMerged(merged, model));
}

void CheckOrderIndependent(int64_t qty1, int64_t price1, int64_t disc1,
                           int64_t tax1, int64_t qty2, int64_t price2,
                           int64_t disc2, int64_t tax2) {
  Q1DayGroupStats ab{};
  ApplyQ1RowContribution(ab, qty1, price1, disc1, tax1, 1);
  ApplyQ1RowContribution(ab, qty2, price2, disc2, tax2, 1);

  Q1DayGroupStats ba{};
  ApplyQ1RowContribution(ba, qty2, price2, disc2, tax2, 1);
  ApplyQ1RowContribution(ba, qty1, price1, disc1, tax1, 1);

  assert(ab.count == ba.count);
  assert(ab.sum_qty == ba.sum_qty);
  assert(ab.sum_base_price == ba.sum_base_price);
  assert(ab.sum_disc_price == ba.sum_disc_price);
  assert(ab.sum_charge == ba.sum_charge);
  assert(ab.sum_discount == ba.sum_discount);
}

struct Row {
  int64_t qty, price, disc, tax;
};

void CheckFoldShuffle(const std::vector<Row> &rows, uint32_t seed) {
  Q1DayGroupStats direct{};
  for (const auto &r : rows)
    ApplyQ1RowContribution(direct, r.qty, r.price, r.disc, r.tax, 1);

  std::vector<Row> shuffled = rows;
  std::mt19937 rng(seed);
  std::shuffle(shuffled.begin(), shuffled.end(), rng);

  Q1DayGroupStats via{};
  for (const auto &r : shuffled)
    ApplyQ1RowContribution(via, r.qty, r.price, r.disc, r.tax, 1);

  assert(direct.count == via.count);
  assert(direct.sum_qty == via.sum_qty);
  assert(direct.sum_base_price == via.sum_base_price);
  assert(direct.sum_disc_price == via.sum_disc_price);
  assert(direct.sum_charge == via.sum_charge);
  assert(direct.sum_discount == via.sum_discount);
}

void CheckPackUnpack(char rf, char ls) {
  const Q1GroupKey key = PackQ1GroupKey(rf, ls);
  assert(UnpackQ1Returnflag(key) == std::string(1, rf));
  assert(UnpackQ1Linestatus(key) == std::string(1, ls));
  assert(static_cast<uint8_t>(key >> 8) == static_cast<uint8_t>(rf));
  assert(static_cast<uint8_t>(key & 0xFF) == static_cast<uint8_t>(ls));
}

void CheckFinalizeWithAvgs(int64_t qty1, int64_t price1, int64_t disc1,
                           int64_t tax1, int64_t qty2, int64_t price2,
                           int64_t disc2, int64_t tax2, char rf, char ls) {
  Q1DayGroupStats d1{};
  Q1DayGroupStats d2{};
  ApplyQ1RowContribution(d1, qty1, price1, disc1, tax1, 1);
  ApplyQ1RowContribution(d2, qty2, price2, disc2, tax2, 1);

  Q1MergedGroupStats merged{};
  MergeQ1DayGroupStats(merged, d1);
  MergeQ1DayGroupStats(merged, d2);
  assert(merged.count > 0);

  std::unordered_map<Q1GroupKey, Q1MergedGroupStats> map;
  const Q1GroupKey key = PackQ1GroupKey(rf, ls);
  map.emplace(key, merged);

  Q1MergedGroupStats empty{};
  map.emplace(PackQ1GroupKey('X', 'Y'), empty);

  const std::vector<Q1OutputRow> rows = FinalizeQ1MergedGroups(map);
  assert(rows.size() == 1);
  assert(rows[0].group_key == key);
  assert(rows[0].count_order == merged.count);
  assert(HugeintAsI64(rows[0].sum_qty) == static_cast<int64_t>(merged.sum_qty));
  assert(HugeintAsI64(rows[0].sum_base_price) ==
         static_cast<int64_t>(merged.sum_base_price));

  const uint64_t count = static_cast<uint64_t>(merged.count);
  const double model_avg_qty =
      ModelHugeintScaleToDouble(merged.sum_qty, 2, count);
  const double model_avg_price =
      ModelHugeintScaleToDouble(merged.sum_base_price, 2, count);
  const double model_avg_disc =
      ModelHugeintScaleToDouble(merged.sum_discount, 2, count);

  // Within TPC-H-ish domains AccToHugeint+Cast matches static_cast<double> on
  // the int64/__int128 values we produce; require exact bit match to C++ avgs.
  assert(DoubleBitsEqual(rows[0].avg_qty, model_avg_qty));
  assert(DoubleBitsEqual(rows[0].avg_price, model_avg_price));
  assert(DoubleBitsEqual(rows[0].avg_disc, model_avg_disc));
}

void CheckEncodeDecode(char rf, char ls, int64_t qty, int64_t price,
                       int64_t disc, int64_t tax) {
  Q1DayPartition day;
  auto &group = day.groups[PackQ1GroupKey(rf, ls)];
  ApplyQ1RowContribution(group, qty, price, disc, tax, 1);

  const auto bytes = EncodeTpchQ1Day(day);
  Q1DayPartition decoded;
  assert(DecodeTpchQ1Day(bytes, decoded));
  assert(decoded.groups.size() == day.groups.size());
  for (const auto &entry : day.groups) {
    auto it = decoded.groups.find(entry.first);
    assert(it != decoded.groups.end());
    assert(GroupEqual(entry.second, it->second));
  }
}

void CheckScaleToRawTwin() {
  // Identity @ scale 2 (Q1ModuleTwin.ScaleToRawIdentity)
  assert(ModelScaleToRaw(12345, 2, 2) == 12345);
  assert(ModelScaleToRaw(100, 0, 2) == 10000);
  assert(ModelScaleToRaw(10000, 4, 2) == 100);
  assert(ModelScaleToRaw(7, 1, 3) == 700);
}

void CheckCutoffDays() {
  // Twin: cutoff = base - offset. Public TpchQ1CutoffDays uses fixed base.
  for (int32_t offset = 0; offset <= 120; offset += 15) {
    const int32_t a = TpchQ1CutoffDays(offset);
    const int32_t b = TpchQ1CutoffDays(offset + 1);
    assert(b == a - 1);
  }
}

void CheckTxnBuffer() {
  TpchQ1TxnBuffer buf;
  assert(!buf.HasPendingChanges());
  assert(!buf.RequiresRebuild());
  assert(buf.Deltas().empty());

  accelerator_codegen::aux_state::query::TpchQ1LineitemDelta d{};
  d.shipdate_days = 100;
  d.group_key = PackQ1GroupKey('N', 'O');
  d.quantity_raw = 1000;
  d.sign = 1;
  buf.AddDelta(d);
  assert(buf.HasPendingChanges());
  assert(buf.Deltas().size() == 1);

  buf.MarkRebuild();
  assert(buf.RequiresRebuild());
  assert(buf.Deltas().empty());

  buf.Clear();
  assert(!buf.HasPendingChanges());
  assert(!buf.RequiresRebuild());
}

AuxRow MakeLineitemAuxRow(int32_t shipdate_days, int64_t qty, int64_t price,
                          int64_t disc, int64_t tax, char rf, char ls) {
  return {{AuxValue::Of(AuxType::Date, AuxDate{shipdate_days}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{qty, 2}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{price, 2}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{disc, 2}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{tax, 2}),
           AuxValue::Of(AuxType::String, std::string(1, rf)),
           AuxValue::Of(AuxType::String, std::string(1, ls))}};
}

// Minimal AuxEngineServices stub for AccumulateMutation (ignored for inserts).
class MinimalServices final
    : public accelerator_codegen::aux_state::core::AuxEngineServices {
public:
  accelerator_codegen::aux_state::core::VisibilityToken
  CurrentVisibility() const override {
    return {};
  }
  accelerator_codegen::aux_state::core::AuxStateStore &StateStore() override {
    throw std::logic_error("unused");
  }
};

void CheckAccumulateInsertAndIrrelevant() {
  TpchQ1Module module(TpchQ1Config{/*lineitem_table=*/7});
  MinimalServices services;

  auto base = module.CreateTxnBuffer();
  auto &buf = dynamic_cast<TpchQ1TxnBuffer &>(*base);

  // Irrelevant table → no-op
  MutationBatch other;
  other.table = 99;
  other.kind = MutationKind::Insert;
  other.new_rows.push_back(
      MakeLineitemAuxRow(10000, 1000, 10000, 5, 4, 'N', 'O'));
  module.AccumulateMutation(other, services, buf);
  assert(!buf.HasPendingChanges());
  assert(buf.Deltas().empty());

  MutationBatch ins;
  ins.table = 7;
  ins.kind = MutationKind::Insert;
  ins.new_rows.push_back(
      MakeLineitemAuxRow(10000, 1000, 10000, 5, 4, 'N', 'O'));
  module.AccumulateMutation(ins, services, buf);
  assert(buf.HasPendingChanges());
  assert(buf.Deltas().size() == 1);
  assert(buf.Deltas()[0].sign == 1);
  assert(buf.Deltas()[0].group_key == PackQ1GroupKey('N', 'O'));
  assert(buf.Deltas()[0].quantity_raw == 1000);

  // Same-day update → two deltas
  MutationBatch upd;
  upd.table = 7;
  upd.kind = MutationKind::Update;
  upd.old_rows.push_back(
      MakeLineitemAuxRow(10000, 1000, 10000, 5, 4, 'N', 'O'));
  upd.new_rows.push_back(
      MakeLineitemAuxRow(10000, 2000, 20000, 6, 4, 'N', 'O'));
  auto base2 = module.CreateTxnBuffer();
  auto &buf2 = dynamic_cast<TpchQ1TxnBuffer &>(*base2);
  module.AccumulateMutation(upd, services, buf2);
  assert(!buf2.RequiresRebuild());
  assert(buf2.Deltas().size() == 2);
  assert(buf2.Deltas()[0].sign == -1);
  assert(buf2.Deltas()[1].sign == 1);

  // Cross-day update → rebuild
  MutationBatch cross;
  cross.table = 7;
  cross.kind = MutationKind::Update;
  cross.old_rows.push_back(
      MakeLineitemAuxRow(10000, 1000, 10000, 5, 4, 'N', 'O'));
  cross.new_rows.push_back(
      MakeLineitemAuxRow(10001, 1000, 10000, 5, 4, 'N', 'O'));
  auto base3 = module.CreateTxnBuffer();
  auto &buf3 = dynamic_cast<TpchQ1TxnBuffer &>(*base3);
  module.AccumulateMutation(cross, services, buf3);
  assert(buf3.RequiresRebuild());
  assert(buf3.Deltas().empty());

  // InterpretMutation
  auto plan = module.InterpretMutation(ins);
  assert(plan.scope ==
         accelerator_codegen::aux_state::InvalidationScope::PartitionSet);
  auto plan2 = module.InterpretMutation(other);
  assert(plan2.scope ==
         accelerator_codegen::aux_state::InvalidationScope::None);
}

} // namespace

int main() {
  for (int64_t qty = 100; qty <= 5000; qty += 700) {
    for (int64_t price = 100; price <= 100000; price += 15000) {
      for (int64_t disc = 0; disc <= 10; ++disc) {
        for (int64_t tax = 0; tax <= 10; tax += 2) {
          CheckApply(qty, price, disc, tax, 1);
          CheckApply(qty, price, disc, tax, -1);
          CheckApplyThenCancel(qty, price, disc, tax);
        }
      }
    }
  }

  const int64_t samples[][4] = {
      {1000, 10000, 5, 4}, {2000, 20000, 6, 8}, {500, 5000, 0, 0},
      {4900, 99000, 10, 10}, {1500, 33300, 3, 7},
  };
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    for (size_t j = 0; j < sizeof(samples) / sizeof(samples[0]); ++j) {
      CheckOrderIndependent(samples[i][0], samples[i][1], samples[i][2],
                            samples[i][3], samples[j][0], samples[j][1],
                            samples[j][2], samples[j][3]);
      CheckMerge(samples[i][0], samples[i][1], samples[i][2], samples[i][3],
                 samples[j][0], samples[j][1], samples[j][2], samples[j][3]);
      CheckFinalizeWithAvgs(samples[i][0], samples[i][1], samples[i][2],
                            samples[i][3], samples[j][0], samples[j][1],
                            samples[j][2], samples[j][3], 'N', 'O');
    }
  }

  std::vector<Row> fold_rows = {
      {1000, 10000, 5, 4}, {2000, 15000, 6, 2}, {3000, 8000, 1, 8},
      {500, 12000, 9, 0},  {2500, 50000, 4, 5},
  };
  for (uint32_t seed = 1; seed <= 32; ++seed)
    CheckFoldShuffle(fold_rows, seed);

  for (char rf : {'A', 'N', 'R'})
    for (char ls : {'O', 'F'})
      CheckPackUnpack(rf, ls);

  CheckEncodeDecode('N', 'O', 1000, 10000, 5, 4);
  CheckEncodeDecode('A', 'F', 2500, 50000, 4, 5);
  CheckEncodeDecode('R', 'O', 500, 12000, 9, 0);

  CheckScaleToRawTwin();
  CheckCutoffDays();
  CheckTxnBuffer();
  CheckAccumulateInsertAndIrrelevant();

  return 0;
}
