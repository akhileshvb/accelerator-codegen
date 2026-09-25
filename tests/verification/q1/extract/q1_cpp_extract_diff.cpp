// Dump generated finalize.cpp / module.cpp over the same grid D as
// Q1ExtractMain.dfy. Keep literals in sync with that file.
//
// Grid D:
//   flags {A,N,R}×{O,F}  (ASCII 65/78/82 × 79/70)
//   apply (qty,price,disc,tax,sign):
//     (1000,10000,5,4,+1), (100,100,0,0,+1), (5000,100000,10,10,+1)
//     then cancel (1000,10000,5,4,-1) after the first
//   cutoff offsets {0,60,90,120}
//   ScaleToRaw (raw,scale)→target 2: (1000,0), (1000,2), (100000,4)
//   shipdates DaysFromCivil(1995,6,15), (1995,6,16); cutoff+1 excluded
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"
#include "in_memory_store.hpp"
#include "row_sink.hpp"
#include "tpch/q1/postgres/aux/finalize.hpp"
#include "tpch/q1/postgres/aux/module.hpp"

using accelerator_codegen::ApplyQ1RowContribution;
using accelerator_codegen::FinalizeQ1MergedGroups;
using accelerator_codegen::MergeQ1DayGroupStats;
using accelerator_codegen::PackQ1GroupKey;
using accelerator_codegen::Q1DayGroupStats;
using accelerator_codegen::Q1DayPartition;
using accelerator_codegen::Q1GroupKey;
using accelerator_codegen::Q1MergedGroupStats;
using accelerator_codegen::Q1OutputRow;
using accelerator_codegen::UnpackQ1Linestatus;
using accelerator_codegen::UnpackQ1Returnflag;
using accelerator_codegen::aux_state::InvalidationScope;
using accelerator_codegen::aux_state::core::AuxDate;
using accelerator_codegen::aux_state::core::AuxEngineServices;
using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::MaintenanceDecision;
using accelerator_codegen::aux_state::core::MaintenanceRequest;
using accelerator_codegen::aux_state::core::MutationBatch;
using accelerator_codegen::aux_state::core::MutationKind;
using accelerator_codegen::aux_state::core::QueryArguments;
using accelerator_codegen::aux_state::core::ScaledDecimal;
using accelerator_codegen::aux_state::core::VisibilityToken;
using accelerator_codegen::aux_state::maintenance::CostModelParams;
using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
using accelerator_codegen::aux_state::query::DecodeTpchQ1Day;
using accelerator_codegen::aux_state::query::EncodeTpchQ1Day;
using accelerator_codegen::aux_state::query::TpchQ1Config;
using accelerator_codegen::aux_state::query::TpchQ1CutoffDays;
using accelerator_codegen::aux_state::query::TpchQ1InitialScan;
using accelerator_codegen::aux_state::query::TpchQ1LineitemDelta;
using accelerator_codegen::aux_state::query::TpchQ1LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ1Module;
using accelerator_codegen::aux_state::query::TpchQ1StateImage;
using accelerator_codegen::aux_state::query::TpchQ1TxnBuffer;

namespace {

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

int AsInt(char c) { return static_cast<int>(static_cast<unsigned char>(c)); }

int64_t HugeLower(const duckdb::hugeint_t &value) {
  return static_cast<int64_t>(value.lower);
}

const char *ScopeName(InvalidationScope scope) {
  switch (scope) {
  case InvalidationScope::None:
    return "None";
  case InvalidationScope::PartitionSet:
    return "PartitionSet";
  case InvalidationScope::AllPartitions:
    return "All";
  }
  return "?";
}

void DumpPack(char rf, char ls) {
  const auto k = PackQ1GroupKey(rf, ls);
  std::cout << "pack " << AsInt(rf) << " " << AsInt(ls) << " -> " << k << "\n";
  const auto urf = UnpackQ1Returnflag(k);
  const auto uls = UnpackQ1Linestatus(k);
  std::cout << "unpack " << k << " rf=" << AsInt(urf.empty() ? 0 : urf[0])
            << " ls=" << AsInt(uls.empty() ? 0 : uls[0]) << "\n";
}

void DumpDayStats(const char *tag, const Q1DayGroupStats &s) {
  std::cout << tag << " count=" << s.count << " qty=" << s.sum_qty
            << " base=" << s.sum_base_price
            << " discp=" << static_cast<int64_t>(s.sum_disc_price)
            << " charge=" << static_cast<int64_t>(s.sum_charge)
            << " disc=" << static_cast<int64_t>(s.sum_discount) << "\n";
}

void DumpMerged(const char *tag, const Q1MergedGroupStats &s) {
  std::cout << tag << " count=" << s.count
            << " qty=" << static_cast<int64_t>(s.sum_qty)
            << " base=" << static_cast<int64_t>(s.sum_base_price)
            << " discp=" << static_cast<int64_t>(s.sum_disc_price)
            << " charge=" << static_cast<int64_t>(s.sum_charge)
            << " disc=" << static_cast<int64_t>(s.sum_discount) << "\n";
}

void DumpAvg(const std::string &tag, double value) {
  std::cout << "AVG " << tag << " " << std::setprecision(17) << value << "\n";
}

void DumpOutRow(const char *tag, const Q1OutputRow &r) {
  std::cout << tag << " gkey=" << r.group_key << " qty=" << HugeLower(r.sum_qty)
            << " base=" << HugeLower(r.sum_base_price)
            << " discp=" << HugeLower(r.sum_disc_price)
            << " charge=" << HugeLower(r.sum_charge)
            << " count=" << r.count_order << "\n";
  DumpAvg(std::string(tag) + "_qty", r.avg_qty);
  DumpAvg(std::string(tag) + "_price", r.avg_price);
  DumpAvg(std::string(tag) + "_disc", r.avg_disc);
}

void DumpPartCanonical(const char *tag, const Q1DayPartition &part) {
  std::vector<Q1GroupKey> keys;
  keys.reserve(part.groups.size());
  for (const auto &entry : part.groups)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  std::cout << tag << " ngroups=" << keys.size() << "\n";
  for (const auto k : keys) {
    const auto &s = part.groups.at(k);
    std::cout << tag << " gkey=" << k << " count=" << s.count
              << " qty=" << s.sum_qty << " base=" << s.sum_base_price
              << " discp=" << static_cast<int64_t>(s.sum_disc_price)
              << " charge=" << static_cast<int64_t>(s.sum_charge)
              << " disc=" << static_cast<int64_t>(s.sum_discount) << "\n";
  }
}

void DumpTxn(const char *tag, const TpchQ1TxnBuffer &buf) {
  std::cout << tag << " pending=" << (buf.HasPendingChanges() ? 1 : 0)
            << " rebuild=" << (buf.RequiresRebuild() ? 1 : 0)
            << " ndeltas=" << buf.Deltas().size() << "\n";
}

void DumpDelta(const char *tag, const TpchQ1LineitemDelta &d) {
  std::cout << tag << " day=" << d.shipdate_days << " gkey=" << d.group_key
            << " qty=" << d.quantity_raw << " price=" << d.extended_price_raw
            << " disc=" << d.discount_raw << " tax=" << d.tax_raw
            << " sign=" << static_cast<int>(d.sign) << "\n";
}

AuxRow MakeLineitemAuxRow(int32_t shipdate_days, int64_t qty, int32_t qty_scale,
                          int64_t price, int32_t price_scale, int64_t disc,
                          int32_t disc_scale, int64_t tax, int32_t tax_scale,
                          char rf, char ls) {
  return {{AuxValue::Of(AuxType::Date, AuxDate{shipdate_days}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{qty, qty_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{price, price_scale}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{disc, disc_scale}),
           AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{tax, tax_scale}),
           AuxValue::Of(AuxType::String, std::string(1, rf)),
           AuxValue::Of(AuxType::String, std::string(1, ls))}};
}

class HarnessServices final : public AuxEngineServices, public TpchQ1InitialScan {
public:
  accel_verify::InMemoryStore store{"tpch_q1"};
  std::vector<TpchQ1LineitemInput> lineitems;
  MaintenanceAction forced = MaintenanceAction::ApplyDelta;
  CostModelParams costs;

  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer(row);
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  accelerator_codegen::aux_state::core::AuxStateStore &StateStore() override {
    return store;
  }
  const CostModelParams &MaintenanceCostModel() const override { return costs; }
  double ExpectedQueriesBeforeNextWrite() const override { return 10.0; }
  bool AllowsSynchronousRebuildOnCommit() const override { return true; }
  MaintenanceDecision
  ChooseMaintenance(const MaintenanceRequest &) const override {
    MaintenanceDecision d;
    d.action = forced;
    return d;
  }
  std::size_t WorkerCount() const override { return 1; }
};

void DumpSink(const char *tag, const accel_verify::CollectingSink &sink) {
  std::cout << tag << " nrows=" << sink.rows.size() << "\n";
  for (const auto &row : sink.rows) {
    const auto rf = std::get<std::string>(row.values[0].payload);
    const auto ls = std::get<std::string>(row.values[1].payload);
    const auto qty = std::get<ScaledDecimal>(row.values[2].payload);
    const auto base = std::get<ScaledDecimal>(row.values[3].payload);
    const auto discp = std::get<ScaledDecimal>(row.values[4].payload);
    const auto charge = std::get<ScaledDecimal>(row.values[5].payload);
    const auto avg_qty = std::get<double>(row.values[6].payload);
    const auto avg_price = std::get<double>(row.values[7].payload);
    const auto avg_disc = std::get<double>(row.values[8].payload);
    const auto count = std::get<int64_t>(row.values[9].payload);
    std::cout << tag << " rf=" << AsInt(rf.empty() ? 0 : rf[0])
              << " ls=" << AsInt(ls.empty() ? 0 : ls[0]) << " qty=" << qty.value
              << " base=" << base.value << " discp=" << discp.value
              << " charge=" << charge.value << " count=" << count << "\n";
    DumpAvg(std::string(tag) + "_qty", avg_qty);
    DumpAvg(std::string(tag) + "_price", avg_price);
    DumpAvg(std::string(tag) + "_disc", avg_disc);
  }
}

const Q1DayPartition *PartitionFromImage(
    const accelerator_codegen::aux_state::core::StateImage &image) {
  const auto *typed = image.As<TpchQ1StateImage>();
  if (!typed || !typed->partition)
    return nullptr;
  return typed->partition.get();
}

} // namespace

int main() {
  DumpPack('A', 'O');
  DumpPack('A', 'F');
  DumpPack('N', 'O');
  DumpPack('N', 'F');
  DumpPack('R', 'O');
  DumpPack('R', 'F');

  Q1DayGroupStats s0{};
  Q1DayGroupStats s1 = s0;
  ApplyQ1RowContribution(s1, 1000, 10000, 5, 4, 1);
  DumpDayStats("apply", s1);
  Q1DayGroupStats s2 = s0;
  ApplyQ1RowContribution(s2, 100, 100, 0, 0, 1);
  DumpDayStats("apply", s2);
  Q1DayGroupStats s3 = s0;
  ApplyQ1RowContribution(s3, 5000, 100000, 10, 10, 1);
  DumpDayStats("apply", s3);
  Q1DayGroupStats sc = s1;
  ApplyQ1RowContribution(sc, 1000, 10000, 5, 4, -1);
  DumpDayStats("apply_cancel", sc);

  Q1MergedGroupStats merged{};
  MergeQ1DayGroupStats(merged, s1);
  MergeQ1DayGroupStats(merged, s2);
  DumpMerged("merge", merged);

  std::unordered_map<Q1GroupKey, Q1MergedGroupStats> fin_map;
  Q1MergedGroupStats m_no{};
  MergeQ1DayGroupStats(m_no, s1);
  Q1MergedGroupStats m_af{};
  MergeQ1DayGroupStats(m_af, s3);
  fin_map[PackQ1GroupKey('N', 'O')] = m_no;
  fin_map[PackQ1GroupKey('A', 'F')] = m_af;
  fin_map[0] = Q1MergedGroupStats{};
  const auto fin = FinalizeQ1MergedGroups(fin_map);
  std::cout << "finalize nrows=" << fin.size() << "\n";
  for (const auto &row : fin)
    DumpOutRow("finalize", row);

  std::cout << "cutoff offset=0 days=" << TpchQ1CutoffDays(0) << "\n";
  std::cout << "cutoff offset=60 days=" << TpchQ1CutoffDays(60) << "\n";
  std::cout << "cutoff offset=90 days=" << TpchQ1CutoffDays(90) << "\n";
  std::cout << "cutoff offset=120 days=" << TpchQ1CutoffDays(120) << "\n";

  const int32_t ship = DaysFromCivil(1995, 6, 15);
  HarnessServices scale_services;
  TpchQ1Module scale_mod(TpchQ1Config{1}, &scale_services);
  auto dump_scale_qty = [&](int64_t raw, int32_t scale) {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Insert;
    mut.new_rows = {MakeLineitemAuxRow(ship, raw, scale, 10000, 2, 5, 2, 4, 2,
                                       'N', 'O')};
    scale_mod.AccumulateMutation(mut, scale_services, buf);
    std::cout << "scale qty raw=" << raw << " scale=" << scale << " -> "
              << buf.Deltas().at(0).quantity_raw << "\n";
  };
  dump_scale_qty(1000, 0);
  dump_scale_qty(1000, 2);
  dump_scale_qty(100000, 4);

  auto dump_scale_price = [&](int64_t raw, int32_t scale) {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Insert;
    mut.new_rows = {MakeLineitemAuxRow(ship, 1000, 2, raw, scale, 5, 2, 4, 2,
                                       'N', 'O')};
    scale_mod.AccumulateMutation(mut, scale_services, buf);
    std::cout << "scale price raw=" << raw << " scale=" << scale << " -> "
              << buf.Deltas().at(0).extended_price_raw << "\n";
  };
  dump_scale_price(10000, 0);
  dump_scale_price(10000, 2);
  dump_scale_price(1000000, 4);

  const auto k_no = PackQ1GroupKey('N', 'O');
  TpchQ1TxnBuffer tb;
  DumpTxn("txn_empty", tb);
  tb.AddDelta(TpchQ1LineitemDelta{ship, k_no, 1000, 10000, 5, 4, 1});
  DumpTxn("txn_add", tb);
  tb.MarkRebuild();
  DumpTxn("txn_rebuild", tb);
  tb.Clear();
  DumpTxn("txn_clear", tb);

  TpchQ1Module module(TpchQ1Config{1}, &scale_services);
  MutationBatch li;
  li.table = 1;
  std::cout << "interpret table=1 scope="
            << ScopeName(module.InterpretMutation(li).scope) << "\n";
  MutationBatch other;
  other.table = 2;
  std::cout << "interpret table=2 scope="
            << ScopeName(module.InterpretMutation(other).scope) << "\n";

  auto accum = [&](const char *tag, MutationKind kind,
                   const std::vector<AuxRow> &old_rows,
                   const std::vector<AuxRow> &new_rows) {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = kind;
    mut.old_rows = old_rows;
    mut.new_rows = new_rows;
    module.AccumulateMutation(mut, scale_services, buf);
    DumpTxn(tag, buf);
    for (const auto &d : buf.Deltas())
      DumpDelta(tag, d);
  };

  const auto row_ins =
      MakeLineitemAuxRow(ship, 1000, 2, 10000, 2, 5, 2, 4, 2, 'N', 'O');
  const auto row_new =
      MakeLineitemAuxRow(ship, 1100, 2, 11000, 2, 6, 2, 4, 2, 'N', 'O');
  const auto row_day2 =
      MakeLineitemAuxRow(ship + 1, 1100, 2, 11000, 2, 6, 2, 4, 2, 'N', 'O');
  accum("accum_insert", MutationKind::Insert, {}, {row_ins});
  accum("accum_delete", MutationKind::Delete, {row_ins}, {});
  accum("accum_update", MutationKind::Update, {row_ins}, {row_new});
  {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Update;
    mut.old_rows = {row_ins};
    mut.new_rows = {row_day2};
    module.AccumulateMutation(mut, scale_services, buf);
    DumpTxn("accum_update_day", buf);
  }
  {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 2;
    mut.kind = MutationKind::Insert;
    mut.new_rows = {row_ins};
    module.AccumulateMutation(mut, scale_services, buf);
    DumpTxn("accum_other", buf);
  }
  {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Insert;
    mut.new_rows.push_back(AuxRow{});
    module.AccumulateMutation(mut, scale_services, buf);
    DumpTxn("accum_bad", buf);
  }

  Q1DayPartition part;
  ApplyQ1RowContribution(part.groups[PackQ1GroupKey('N', 'O')], 1000, 10000, 5,
                         4, 1);
  ApplyQ1RowContribution(part.groups[PackQ1GroupKey('A', 'F')], 2000, 20000, 6,
                         3, 1);
  DumpPartCanonical("encode", part);
  const auto bytes = EncodeTpchQ1Day(part);
  Q1DayPartition decoded;
  DecodeTpchQ1Day(bytes, decoded);
  DumpPartCanonical("decode", decoded);

  HarnessServices compose_services;
  TpchQ1LineitemInput in_no{};
  in_no.shipdate_days = ship;
  in_no.quantity_raw = 1000;
  in_no.quantity_scale = 2;
  in_no.extended_price_raw = 10000;
  in_no.extended_price_scale = 2;
  in_no.discount_raw = 5;
  in_no.discount_scale = 2;
  in_no.tax_raw = 4;
  in_no.tax_scale = 2;
  in_no.returnflag = 'N';
  in_no.linestatus = 'O';
  TpchQ1LineitemInput in2 = in_no;
  in2.shipdate_days = DaysFromCivil(1995, 6, 16);
  in2.quantity_raw = 2000;
  in2.extended_price_raw = 20000;
  in2.discount_raw = 6;
  in2.tax_raw = 3;
  in2.returnflag = 'A';
  in2.linestatus = 'F';
  TpchQ1LineitemInput in_past = in_no;
  in_past.shipdate_days = TpchQ1CutoffDays(0) + 1;
  in_past.quantity_raw = 3000;
  in_past.extended_price_raw = 30000;
  in_past.discount_raw = 7;
  in_past.tax_raw = 2;
  in_past.returnflag = 'R';
  in_past.linestatus = 'O';
  compose_services.lineitems = {in_no, in2, in_past};
  TpchQ1Module compose_mod(TpchQ1Config{1}, &compose_services);
  QueryArguments args{
      {AuxValue::Of(AuxType::Int64, static_cast<int64_t>(0))}};
  accel_verify::CollectingSink sink;
  compose_mod.ExecuteFallback(compose_services, args, sink);
  DumpSink("compose", sink);

  HarnessServices stage_services;
  TpchQ1Module stage_mod(TpchQ1Config{1}, &stage_services);
  stage_services.forced = MaintenanceAction::ApplyDelta;
  {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Insert;
    mut.new_rows = {row_ins};
    stage_mod.AccumulateMutation(mut, stage_services, buf);
    const auto outcome = stage_mod.StageMaintenance(stage_services, {}, buf);
    Q1DayPartition after;
    for (const auto &image : outcome.updates) {
      const auto *p = PartitionFromImage(image);
      if (p)
        after = *p;
    }
    DumpPartCanonical("stage_insert", after);
    stage_services.store.InstallBuild(outcome.updates);
  }
  {
    TpchQ1TxnBuffer buf;
    MutationBatch mut;
    mut.table = 1;
    mut.kind = MutationKind::Delete;
    mut.old_rows = {row_ins};
    stage_mod.AccumulateMutation(mut, stage_services, buf);
    const auto outcome = stage_mod.StageMaintenance(stage_services, {}, buf);
    Q1DayPartition after;
    for (const auto &image : outcome.updates) {
      const auto *p = PartitionFromImage(image);
      if (p)
        after = *p;
    }
    DumpPartCanonical("stage_delete", after);
  }
  return 0;
}
