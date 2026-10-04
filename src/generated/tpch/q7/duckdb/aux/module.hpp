#pragma once

#include <cstdint>
#include <functional>
#include <algorithm>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "tpch/q7/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ7Config {
  core::TableId nation_table = 0;
  core::TableId supplier_table = 0;
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ7NationInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ7SupplierInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7CustomerInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
};

struct TpchQ7LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
};

class TpchQ7InitialScan {
public:
  virtual ~TpchQ7InitialScan() = default;
  virtual void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &) = 0;

  // Optional fast paths (engine pushes the join + GROUP BY into SQL). Return
  // false when unsupported; BuildState then falls back to ScanLineitems.
  // Row count of the orders table, or -1 if unknown (sizes the page table).
  virtual int64_t CountOrders() { return -1; }
  // revenue(supp_nationkey, cust_nationkey, year, sum of price*(100-disc_raw))
  // over lineitems in [1995-01-01,1996-12-31] joined to supplier/orders/
  // customer; supplier_counts(suppkey, #window lineitems referencing it);
  // customer_counts(custkey, #window lineitems whose order has that custkey).
  virtual bool ScanWindowAggregates(
      const std::function<void(int32_t, int32_t, int32_t, int64_t)> &,
      const std::function<void(int64_t, int64_t)> &,
      const std::function<void(int64_t, int64_t)> &) {
    return false;
  }
};

constexpr int32_t kTpchQ7MinNationSlots = 25;
constexpr int32_t kTpchQ7MinYear = 1995;
constexpr int32_t kTpchQ7MaxYear = 1996;

// Copy-on-write hash-paged sorted map. Pages are immutable once published;
// With() copies only the pages a change touches and shares the rest.
template <class V> class PagedMap {
public:
  struct Entry {
    int64_t key;
    V val;
  };
  using Page = std::vector<Entry>;
  static constexpr std::size_t kTargetPage = 4096;
  static constexpr std::size_t kMaxPages = 1u << 16;

  static std::size_t PagesFor(std::size_t n) {
    std::size_t p = 1;
    while (p < kMaxPages && n / p > kTargetPage)
      p <<= 1;
    return p;
  }

  const V *Find(int64_t key) const {
    if (pages.empty())
      return nullptr;
    const Page &p = *pages[Slot(key, mask)];
    const auto it = std::lower_bound(
        p.begin(), p.end(), key,
        [](const Entry &e, int64_t k) { return e.key < k; });
    return (it != p.end() && it->key == key) ? &it->val : nullptr;
  }
  std::size_t Size() const { return total; }
  template <class F> void ForEach(F &&f) const {
    for (const auto &p : pages)
      for (const auto &e : *p)
        f(e.key, e.val);
  }

  // Returns a new map with `changes` applied (nullopt = erase). *this and its
  // pages are untouched; only touched pages are copied.
  PagedMap With(const std::unordered_map<int64_t, std::optional<V>> &changes) const {
    PagedMap out = *this;
    if (out.pages.empty()) {
      out.pages.push_back(std::make_shared<const Page>());
      out.mask = 0;
    }
    std::unordered_map<std::size_t,
                       std::vector<std::pair<int64_t, const std::optional<V> *>>>
        by_page;
    for (const auto &[k, v] : changes)
      by_page[Slot(k, out.mask)].emplace_back(k, &v);
    for (auto &[slot, list] : by_page) {
      std::sort(list.begin(), list.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
      const Page &old = *out.pages[slot];
      auto next = std::make_shared<Page>();
      next->reserve(old.size() + list.size());
      std::size_t i = 0, j = 0;
      while (i < old.size() || j < list.size()) {
        if (j == list.size() || (i < old.size() && old[i].key < list[j].first)) {
          next->push_back(old[i++]);
        } else {
          if (i < old.size() && old[i].key == list[j].first)
            ++i;
          if (list[j].second->has_value())
            next->push_back({list[j].first, **list[j].second});
          ++j;
        }
      }
      out.total = out.total - old.size() + next->size();
      out.pages[slot] = std::shared_ptr<const Page>(std::move(next));
    }
    return out;
  }

  class Builder {
  public:
    explicit Builder(std::size_t expected_entries)
        : pages(PagesFor(expected_entries)), mask(pages.size() - 1) {}
    void Add(int64_t key, const V &val) {
      pages[Slot(key, mask)].push_back({key, val});
    }
    void Sort() {
      for (auto &p : pages)
        std::sort(p.begin(), p.end(), [](const Entry &a, const Entry &b) {
          return a.key < b.key;
        });
    }
    // Valid only after Sort().
    V *FindMut(int64_t key) {
      Page &p = pages[Slot(key, mask)];
      const auto it = std::lower_bound(
          p.begin(), p.end(), key,
          [](const Entry &e, int64_t k) { return e.key < k; });
      return (it != p.end() && it->key == key) ? &it->val : nullptr;
    }
    PagedMap Freeze() {
      PagedMap out;
      out.mask = mask;
      for (auto &p : pages) {
        out.total += p.size();
        out.pages.push_back(std::make_shared<const Page>(std::move(p)));
      }
      pages.clear();
      return out;
    }

  private:
    std::vector<Page> pages;
    std::size_t mask;
  };

private:
  static std::size_t Slot(int64_t key, std::size_t mask) {
    return static_cast<std::size_t>(
               (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL) >> 40) &
           mask;
  }
  std::vector<std::shared_ptr<const Page>> pages;
  std::size_t mask = 0;
  std::size_t total = 0;
};

// nation key plus the number of in-window (1995-1996) lineitems that reference
// the key (conservative: a non-zero count forces a rebuild when the key is
// deleted or its nation changes).
struct TpchQ7NationCount {
  int32_t nation = 0;
  int32_t window_count = 0;
};
struct TpchQ7OrderCust {
  int64_t custkey = 0;
};

struct TpchQ7State {
  std::unordered_map<int32_t, std::string> nation_names;
  PagedMap<TpchQ7NationCount> supplier_nations;
  PagedMap<TpchQ7NationCount> customer_nations;
  // All orders (orderkey -> custkey), paged copy-on-write.
  PagedMap<TpchQ7OrderCust> order_custkeys;
  std::vector<int64_t> revenue_1995;
  std::vector<int64_t> revenue_1996;
  int32_t num_nation_slots = kTpchQ7MinNationSlots;
};

enum class TpchQ7ImageKind : uint8_t { Side, Revenue };

struct TpchQ7StateImage {
  TpchQ7ImageKind kind = TpchQ7ImageKind::Side;
  std::shared_ptr<const TpchQ7State> state;
};

// One captured write, resolved against the published state at StageMaintenance.
struct TpchQ7Op {
  enum Type : uint8_t { Supplier, Customer, Order, Lineitem };
  Type type = Lineitem;
  // Dimension ops: 0 insert, 1 delete, 2 update. Lineitem: +1 / -1.
  int8_t kind = 0;
  // Dimension: (key,val) old = k0,v0; new = k1,v1 (val = nationkey / custkey).
  // Lineitem: k0 = orderkey, v0 = suppkey, k1 = shipdate_days, v1 = revenue raw.
  int64_t k0 = 0, v0 = 0, k1 = 0, v1 = 0;
};

std::vector<uint8_t> EncodeTpchQ7Side(const TpchQ7State &);
bool DecodeTpchQ7Side(const std::vector<uint8_t> &, TpchQ7State &);
std::vector<uint8_t> EncodeTpchQ7Revenue(const TpchQ7State &);
bool DecodeTpchQ7Revenue(const std::vector<uint8_t> &, TpchQ7State &);

int64_t TpchQ7RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale);
int32_t TpchQ7MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ7DaysFromYmd(int32_t year, unsigned month, unsigned day);

class TpchQ7TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ7Op> &Ops() const;
  void MarkRebuild();
  void AddOp(const TpchQ7Op &op);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ7Op> ops;
};

class TpchQ7Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ7Layout::kAccelerator;

  explicit TpchQ7Module(TpchQ7Config config,
                        TpchQ7InitialScan *initial_scan = nullptr)
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
  TpchQ7State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, const std::string &nation1,
                 const std::string &nation2,
                 std::shared_ptr<const TpchQ7State> &side,
                 std::vector<std::pair<int32_t, std::shared_ptr<const TpchQ7State>>> &months,
                 std::map<core::PartitionId, core::Version> *versions = nullptr) const;
  void EmitResult(const std::shared_ptr<const TpchQ7State> &side,
                  const std::shared_ptr<const TpchQ7State> &revenue,
                  const std::string &nation1, const std::string &nation2,
                  core::AuxRowSink &) const;

  TpchQ7Config config;
  TpchQ7InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
