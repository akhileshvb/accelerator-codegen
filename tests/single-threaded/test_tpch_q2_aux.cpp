#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "tpch/q2/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ2Config;
using accelerator_codegen::aux_state::query::TpchQ2InitialScan;
using accelerator_codegen::aux_state::query::TpchQ2Module;
using accelerator_codegen::aux_state::query::TpchQ2PartInput;
using accelerator_codegen::aux_state::query::TpchQ2PartsuppInput;
using accelerator_codegen::aux_state::query::TpchQ2SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ2NationInput;
using accelerator_codegen::aux_state::query::TpchQ2RegionInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}

AuxRow Region(int32_t key, const std::string &name) {
  return {{Int(key), String(name)}};
}
AuxRow Nation(int32_t key, const std::string &name, int32_t region) {
  return {{Int(key), String(name), Int(region)}};
}
AuxRow Part(int64_t key, const std::string &mfgr, const std::string &type, int32_t size) {
  return {{Int(key), String(mfgr), String(type), Int(size)}};
}
AuxRow Supplier(int64_t key, const std::string &name, const std::string &addr,
                int32_t nk, const std::string &phone, int64_t acctbal_raw,
                const std::string &comment) {
  return {{Int(key), String(name), String(addr), Int(nk), String(phone),
           Decimal(acctbal_raw, 2), String(comment)}};
}
AuxRow Partsupp(int64_t partkey, int64_t suppkey, int64_t cost_raw) {
  return {{Int(partkey), Int(suppkey), Decimal(cost_raw, 2)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q2";
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == "tpch_q2" && valid
               ? ValidityResult{ValidityCode::Valid, version}
               : ValidityResult{ValidityCode::Invalidated, 0};
  }
  void Invalidate(const InvalidationPlan &) override { valid = false; }
  bool TryPublish(const StagedMaintenanceOutcome &outcome, Version) override {
    for (const auto &image : outcome.updates) {
      auto it = images.find(image.partition);
      if (!valid || it == images.end() ||
          (image.version != 0 && image.version != it->second.version)) {
        valid = false;
        return false;
      }
    }
    for (const auto &image : outcome.updates) {
      auto &published = images[image.partition];
      published = image;
      published.version++;
    }
    valid = true;
    version++;
    return true;
  }
  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.emplace(image.partition, std::move(image));
    }
    valid = !images.empty();
    version = 1;
  }
  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public TpchQ2InitialScan {
public:
  void ScanParts(const std::function<void(const TpchQ2PartInput &)> &consumer) override {
    for (const auto &row : parts)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
  }
  void ScanSuppliers(const std::function<void(const TpchQ2SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      const auto &acctbal = std::get<ScaledDecimal>(row.values[5].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload)),
                std::get<std::string>(row.values[4].payload),
                acctbal.value,
                std::get<std::string>(row.values[6].payload)});
    }
  }
  void ScanPartsupps(const std::function<void(const TpchQ2PartsuppInput &)> &consumer) override {
    for (const auto &row : partsupps) {
      const auto &cost = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload), cost.value});
    }
  }
  void ScanNations(const std::function<void(const TpchQ2NationInput &)> &consumer) override {
    for (const auto &row : nations)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
  }
  void ScanRegions(const std::function<void(const TpchQ2RegionInput &)> &consumer) override {
    for (const auto &row : regions)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> parts;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> partsupps;
  std::vector<AuxRow> nations;
  std::vector<AuxRow> regions;
};

class Builder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

class Sink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &schema) override {
    rows.clear();
    this->schema = schema;
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<AuxRow> rows;
  OutputSchema schema;
};

struct Q2Row {
  int64_t acctbal_raw = 0;
  std::string supplier_name;
  std::string nation_name;
  int64_t partkey = 0;
  std::string mfgr;
  std::string address;
  std::string phone;
  std::string comment;
};

Q2Row ExtractRow(const AuxRow &row) {
  Q2Row out;
  const auto &acct = std::get<ScaledDecimal>(row.values[0].payload);
  out.acctbal_raw = acct.value;
  out.supplier_name = std::get<std::string>(row.values[1].payload);
  out.nation_name = std::get<std::string>(row.values[2].payload);
  out.partkey = std::get<int64_t>(row.values[3].payload);
  out.mfgr = std::get<std::string>(row.values[4].payload);
  out.address = std::get<std::string>(row.values[5].payload);
  out.phone = std::get<std::string>(row.values[6].payload);
  out.comment = std::get<std::string>(row.values[7].payload);
  return out;
}

void SeedDimension(Services &s) {
  s.regions.clear();
  s.nations.clear();
  s.regions.push_back(Region(1, "R1"));
  s.regions.push_back(Region(2, "R2"));
  s.nations.push_back(Nation(10, "N10", 1));
  s.nations.push_back(Nation(20, "N20", 2));
}

void BuildAndVerify(Services &s, TpchQ2Module &mod, const QueryArguments &args,
                    int expected_rows, const std::string &expected_supplier,
                    int64_t expected_part = 0) {
  Builder builder;
  mod.Build(s, builder);
  assert(builder.finished);
  s.store.InstallBuild(std::move(builder.images));

  Sink hot;
  mod.Execute(s, args, hot);
  assert(hot.rows.size() == static_cast<size_t>(expected_rows));

  Sink fallback;
  mod.ExecuteFallback(s, args, fallback);
  assert(fallback.rows.size() == static_cast<size_t>(expected_rows));

  if (expected_rows > 0 && !expected_supplier.empty()) {
    auto hr = ExtractRow(hot.rows[0]);
    auto fr = ExtractRow(fallback.rows[0]);
    assert(hr.supplier_name == expected_supplier);
    assert(fr.supplier_name == expected_supplier);
    if (expected_part > 0) {
      assert(hr.partkey == expected_part);
      assert(fr.partkey == expected_part);
    }
  }
}

} // namespace

int main() {
  try {
    TpchQ2Config config{1, 2, 3, 4, 5};
    Services s;
    TpchQ2Module mod(config, &s);

    QueryArguments args_brass_r1;
    args_brass_r1.values = {Int(15), String("%BRASS"), String("R1")};

    QueryArguments args_nickel_r1;
    args_nickel_r1.values = {Int(30), String("%NICKEL"), String("R1")};

    QueryArguments args_tin_r1;
    args_tin_r1.values = {Int(42), String("%TIN"), String("R1")};

    QueryArguments args_tin_r1_size30;
    args_tin_r1_size30.values = {Int(30), String("%TIN"), String("R1")};

    // Case 1: Happy path — matching size, type, region; single winner
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(100, "M1", "ECONOMY BRASS", 15));
      s.parts.push_back(Part(101, "M2", "COPPER", 15));
      s.suppliers.push_back(Supplier(1, "S1", "A1", 10, "P1", 10000, "C1"));
      s.suppliers.push_back(Supplier(2, "S2", "A2", 20, "P2", 5000, "C2"));
      s.partsupps.push_back(Partsupp(100, 1, 5000));
      s.partsupps.push_back(Partsupp(100, 2, 3000));
      s.partsupps.push_back(Partsupp(101, 1, 7000));

      BuildAndVerify(s, mod, args_brass_r1, 1, "S1", 100);
    }

    // Case 2: Size mismatch — query size=10, no part with size 10
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(100, "M1", "ECONOMY BRASS", 15));
      s.suppliers.push_back(Supplier(1, "S1", "A1", 10, "P1", 10000, "C1"));
      s.partsupps.push_back(Partsupp(100, 1, 5000));

      BuildAndVerify(s, mod, {{Int(10), String("%BRASS"), String("R1")}}, 0, "");
    }

    // Case 3: Type mismatch — p_type 'COPPER', query '%BRASS' excludes
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(100, "M1", "COPPER", 15));
      s.suppliers.push_back(Supplier(1, "S1", "A1", 10, "P1", 10000, "C1"));
      s.partsupps.push_back(Partsupp(100, 1, 5000));

      BuildAndVerify(s, mod, args_brass_r1, 0, "");
    }

    // Case 4: Supplier outside query region — excluded from MIN
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(100, "M1", "ECONOMY BRASS", 15));
      s.suppliers.push_back(Supplier(1, "S1", "A1", 10, "P1", 10000, "C1"));
      s.suppliers.push_back(Supplier(2, "S2", "A2", 20, "P2", 5000, "C2"));
      s.partsupps.push_back(Partsupp(100, 1, 8000));
      s.partsupps.push_back(Partsupp(100, 2, 1000));

      BuildAndVerify(s, mod, args_brass_r1, 1, "S1");
    }

    // Case 5: Multiple suppliers tied at the minimum in R1
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(100, "M1", "ECONOMY BRASS", 15));
      s.suppliers.push_back(Supplier(1, "S1", "A1", 10, "P1", 10000, "C1"));
      s.suppliers.push_back(Supplier(2, "S2", "A2", 20, "P2", 5000, "C2"));
      s.suppliers.push_back(Supplier(3, "S3", "A3", 10, "P3", 15000, "C3"));
      s.partsupps.push_back(Partsupp(100, 1, 5000));
      s.partsupps.push_back(Partsupp(100, 2, 3000));
      s.partsupps.push_back(Partsupp(100, 3, 5000));

      BuildAndVerify(s, mod, args_brass_r1, 2, "");
    }

    // Case 6: Fallback and hot path agree (NICKEL)
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(300, "M3", "NICKEL", 30));
      s.suppliers.push_back(Supplier(40, "SX", "AX", 10, "PX", 30000, "CX"));
      s.partsupps.push_back(Partsupp(300, 40, 4500));

      BuildAndVerify(s, mod, args_nickel_r1, 1, "SX");
    }

    // Case 7: TIN with size 42 matches
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(200, "M2", "TIN", 42));
      s.suppliers.push_back(Supplier(10, "SA", "AA", 10, "PA", 20000, "CA"));
      s.suppliers.push_back(Supplier(20, "SB", "AB", 10, "PB", 10000, "CB"));
      s.partsupps.push_back(Partsupp(200, 10, 7000));
      s.partsupps.push_back(Partsupp(200, 20, 9000));

      BuildAndVerify(s, mod, args_tin_r1, 1, "SA");
    }

    // Case 8: TIN at size 30 — no match (part is size 42)
    {
      SeedDimension(s);
      s.parts.clear();
      s.suppliers.clear();
      s.partsupps.clear();
      s.parts.push_back(Part(200, "M2", "TIN", 42));
      s.suppliers.push_back(Supplier(10, "SA", "AA", 10, "PA", 20000, "CA"));
      s.partsupps.push_back(Partsupp(200, 10, 7000));

      BuildAndVerify(s, mod, args_tin_r1_size30, 0, "");
    }

    std::cout << "test_tpch_q2_aux passed\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_tpch_q2_aux failed: " << ex.what() << '\n';
    return 1;
  }
}
