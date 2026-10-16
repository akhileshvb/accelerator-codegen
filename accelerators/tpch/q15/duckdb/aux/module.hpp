#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ15Layout {
  static constexpr const char *kAccelerator = "tpch_q15";
  static constexpr core::PartitionId kSupplierDirectory = 0;
  // Month-window partitions are keyed by YYYYMM of the month start.
};

struct TpchQ15Config {
  core::TableId lineitem_table = 0;
  core::TableId supplier_table = 0;
};

struct TpchQ15LineitemInput {
  int32_t shipdate_days = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
};

struct TpchQ15SupplierInput {
  int64_t suppkey = 0;
  std::string name;
  std::string address;
  std::string phone;
};

class TpchQ15InitialScan {
public:
  virtual ~TpchQ15InitialScan() = default;
  // Partitioned scans: rows of one partition arrive in table order on one
  // thread; partitions run concurrently on the pool.
  virtual void ScanLineitems(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ15LineitemInput &)> &) = 0;
  virtual void ScanSuppliers(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ15SupplierInput &)> &) = 0;
  virtual void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const TpchQ15LineitemInput &)> &consumer) = 0;
};

// ── State ─────────────────────────────────────────────────────────────────

struct TpchQ15SupplierRecord {
  std::string name;
  std::string address;
  std::string phone;
};

// Revenue per suppkey: copy-on-write chunks shared between versions (only the
// touched chunks are cloned; a cloned chunk is uniquely owned by the working
// copy until it is published). Absent keys read 0. Keys outside the dense range
// (negative or huge) live in a small ordered overflow map.
struct TpchQ15RevArray {
  static constexpr int64_t kMaxDenseKey = int64_t{1} << 28;
  core::ChunkedCowArray<int64_t, 11> dense{};
  std::map<int64_t, int64_t> overflow;

  // Adds every revenue of `other`.
  void AddAll(const TpchQ15RevArray &other) {
    auto dense_copy = other.dense;
    dense.MergeFrom(std::move(dense_copy), [](int64_t mine, int64_t theirs) { return mine + theirs; });
    for (const auto &[key, revenue] : other.overflow)
      overflow[key] += revenue;
  }
  // Adds delta to the key's revenue; returns the old value.
  int64_t Add(int64_t suppkey, int64_t delta) {
    int64_t &cell = suppkey >= 0 && suppkey < kMaxDenseKey
                        ? dense.Mutable(suppkey)
                        : overflow[suppkey];
    const int64_t old = cell;
    cell += delta;
    return old;
  }
  // fn(suppkey, revenue) for every non-zero entry, ascending by suppkey.
  template <typename F> void ForEachNonZero(F &&fn) const {
    auto it = overflow.begin();
    for (; it != overflow.end() && it->first < 0; ++it)
      if (it->second != 0)
        fn(it->first, it->second);
    for (std::size_t ci = 0; ci < dense.NumChunks(); ++ci) {
      const auto *chunk = dense.ChunkAt(ci);
      if (!chunk)
        continue;
      for (std::size_t i = 0; i < chunk->size(); ++i)
        if ((*chunk)[i] != 0)
          fn(dense.ChunkBase(ci) + static_cast<int64_t>(i), (*chunk)[i]);
    }
    for (; it != overflow.end(); ++it)
      if (it->second != 0)
        fn(it->first, it->second);
  }
};

struct TpchQ15WindowState {
  TpchQ15RevArray rev;
  int64_t nonzero = 0; // suppliers with a non-zero revenue in the window
  bool empty = true;
  int64_t max_revenue = 0;
  std::vector<int64_t> winners; // suppkeys at the max, ascending
  // Recomputes nonzero / max / winners with one pass over the array.
  void Rescan();
};

// Supplier directory: sorted per-chunk entry lists sharing immutable records,
// so a supplier write copies one small chunk and the chunk table.
constexpr int kQ15DirChunkShift = 10;
struct TpchQ15DirChunk {
  std::vector<std::pair<int64_t, std::shared_ptr<const TpchQ15SupplierRecord>>>
      items;
};

struct TpchQ15SupplierDirectory {
  std::map<int64_t, std::shared_ptr<TpchQ15DirChunk>> chunks;
  // Bag semantics: a key may hold several records (duplicate supplier rows).
  // Folds in another directory (bag union; equal keys keep this side first).
  void MergeFrom(TpchQ15SupplierDirectory &&other);
  void FindAll(int64_t suppkey,
               std::vector<const TpchQ15SupplierRecord *> &out) const;
  void Add(int64_t suppkey, TpchQ15SupplierRecord record);
  // Removes one record equal to `record`; no-op when absent.
  void EraseOne(int64_t suppkey, const TpchQ15SupplierRecord &record);
  std::size_t Size() const;
};

struct TpchQ15State {
  TpchQ15SupplierDirectory directory;
  std::unordered_map<int32_t, TpchQ15WindowState> windows;
};

enum class TpchQ15ImageKind : uint8_t {
  SupplierDirectory,
  MonthWindow,
};

struct TpchQ15StateImage {
  TpchQ15ImageKind kind = TpchQ15ImageKind::MonthWindow;
  int32_t month_key = 0;
  std::shared_ptr<const TpchQ15State> state;
};

struct TpchQ15LineitemDelta {
  int32_t month_key = 0;
  int64_t suppkey = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

struct TpchQ15SupplierDelta {
  int64_t suppkey = 0;
  bool has_old = false;
  bool has_new = false;
  std::string old_name;
  std::string old_address;
  std::string old_phone;
  std::string new_name;
  std::string new_address;
  std::string new_phone;
};

// ── Encode / Decode ───────────────────────────────────────────────────────

std::vector<uint8_t> EncodeTpchQ15Directory(const TpchQ15SupplierDirectory &);
std::vector<uint8_t> EncodeTpchQ15Window(int32_t month_key, const TpchQ15WindowState &);
bool DecodeTpchQ15Directory(const std::vector<uint8_t> &, TpchQ15SupplierDirectory &);
bool DecodeTpchQ15Window(const std::vector<uint8_t> &, int32_t month_key,
                         TpchQ15WindowState &);

// ── Helpers ───────────────────────────────────────────────────────────────

int32_t TpchQ15MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ15DaysFromYmd(int32_t year, unsigned month, unsigned day);
bool TpchQ15IsFirstOfMonth(int32_t days_since_epoch);
int32_t TpchQ15AddMonths(int32_t days_since_epoch, int32_t months);
std::vector<int32_t> TpchQ15WindowMonths(int32_t start_days, int32_t end_days);
int64_t TpchQ15RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale);

// ── TxnBuffer ─────────────────────────────────────────────────────────────

class TpchQ15TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ15LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ15SupplierDelta> &SupplierDeltas() const;
  void MarkRebuild();
  void AddLineitemDelta(TpchQ15LineitemDelta delta);
  void AddSupplierDelta(TpchQ15SupplierDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ15LineitemDelta> lineitem_deltas;
  std::vector<TpchQ15SupplierDelta> supplier_deltas;
};

// ── Module ────────────────────────────────────────────────────────────────

class TpchQ15Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ15Layout::kAccelerator;

  explicit TpchQ15Module(TpchQ15Config config,
                         TpchQ15InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ15State BuildState(core::AuxEngineServices &) const;
  void EmitResult(const std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> &,
                  int64_t revenue_raw, core::AuxRowSink &) const;
  bool LoadMonth(core::AuxEngineServices &, int32_t month_key,
                 TpchQ15WindowState &) const;
  bool LoadDirectory(core::AuxEngineServices &, TpchQ15SupplierDirectory &) const;

  TpchQ15Config config;
  TpchQ15InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
