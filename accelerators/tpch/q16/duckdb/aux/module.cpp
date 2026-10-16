#include "tpch/q16/duckdb/aux/module.hpp"
#include "tpch/q16/calibration_writes.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/typed_row_batch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Layout = TpchQ16Layout;

constexpr uint16_t kNoBrand = 0xFFFF;
constexpr uint32_t kNoGroup = 0xFFFFFFFFu;
// More row deltas than this in one commit: a full rebuild is cheaper.
constexpr std::size_t kRebuildOpThreshold = 200000;
constexpr std::size_t kPartPageSize = std::size_t{1} << Layout::kPartShift;
constexpr std::size_t kPsPageSize = std::size_t{1} << Layout::kPsShift;
constexpr std::size_t kSuppPageSize = std::size_t{1} << Layout::kSuppShift;
// Pages per build slice: a partsupp page is ~20us of CSR work, a supplier page a few.
constexpr std::size_t kPsPagesGrain = 16;
constexpr std::size_t kSuppPagesGrain = 64;
// Groups per build slice: counting a group's eligible suppliers is ~1 us per 50 members.
constexpr std::size_t kGroupsGrain = 4096;


// ── Custom profiling (no-op unless ACCEL_AUX_CUSTOM_PROFILE is set) ─────────
struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) / 1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val,
                              int64_t groups, int64_t rows, int64_t suppkeys) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"groups\":%ld,"
          "\"rows\":%ld,"
          "\"suppkeys\":%ld"
          "}\n",
          phase, us_val, (long)groups, (long)rows, (long)suppkeys);
}

// ── Cell helpers ────────────────────────────────────────────────────────────
bool StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload))
    return false;
  out = std::get<std::string>(row.values[index].payload);
  return true;
}

bool IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return true;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    const auto value = std::get<uint64_t>(cell.payload);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(value);
    return true;
  }
  return false;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool IsComplaintComment(const std::string &comment) {
  return comment.find("Customer") != std::string::npos &&
         comment.find("Complaints") != std::string::npos;
}

// ── Row readers ─────────────────────────────────────────────────────────────
bool ReadSupplier(const core::AuxRow &row, TpchQ16SupplierInput &out) {
  int64_t suppkey = 0;
  if (!IntCell(row, 0, suppkey) || !StringCell(row, 1, out.comment))
    return false;
  out.suppkey = suppkey;
  return true;
}

bool ReadPart(const core::AuxRow &row, TpchQ16PartInput &out) {
  int64_t partkey = 0;
  if (!IntCell(row, 0, partkey) || !StringCell(row, 1, out.brand) ||
      !StringCell(row, 2, out.type))
    return false;
  int64_t size = 0;
  if (!IntCell(row, 3, size))
    return false;
  out.partkey = partkey;
  out.size = static_cast<int32_t>(size);
  return true;
}

bool ReadPartsupp(const core::AuxRow &row, TpchQ16PartsuppInput &out) {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  if (!IntCell(row, 0, partkey) || !IntCell(row, 1, suppkey))
    return false;
  out.partkey = partkey;
  out.suppkey = suppkey;
  return true;
}

// ── SQL LIKE helper ────────────────────────────────────────────────────────
bool SqlLikeAt(const char *text, std::size_t text_len, const char *pattern,
               std::size_t pattern_len, std::size_t text_pos,
               std::size_t pattern_pos) {
  while (pattern_pos < pattern_len) {
    const char pattern_ch = pattern[pattern_pos];
    if (pattern_ch == '%') {
      if (pattern_pos + 1 == pattern_len)
        return true;
      for (std::size_t start = text_pos; start <= text_len; ++start) {
        if (SqlLikeAt(text, text_len, pattern, pattern_len, start,
                      pattern_pos + 1))
          return true;
      }
      return false;
    }
    if (pattern_ch == '_') {
      if (text_pos >= text_len)
        return false;
      ++text_pos;
      ++pattern_pos;
      continue;
    }
    if (text_pos >= text_len || text[text_pos] != pattern_ch)
      return false;
    ++text_pos;
    ++pattern_pos;
  }
  return text_pos == text_len;
}

bool SqlLike(const std::string &text, const std::string &pattern) {
  return SqlLikeAt(text.data(), text.size(), pattern.data(), pattern.size(), 0,
                   0);
}


// ── Shared helpers ──────────────────────────────────────────────────────────
inline bool SizeInRange(int32_t size) {
  return size >= Layout::kMinSize && size <= Layout::kMaxSize;
}

inline uint32_t GroupCode(int32_t size, uint32_t brand, uint32_t type) {
  return (static_cast<uint32_t>(size) << 24) | (brand << 16) | type;
}
inline int32_t CodeSize(uint32_t code) { return static_cast<int32_t>(code >> 24); }
inline uint32_t CodeBrand(uint32_t code) { return (code >> 16) & 0xFFu; }
inline uint16_t CodeType(uint32_t code) {
  return static_cast<uint16_t>(code & 0xFFFFu);
}

void CheckKey(int64_t key, const char *what) {
  if (!Layout::KeyInRange(key))
    throw std::runtime_error(std::string("TPC-H Q16: ") + what +
                             " outside the supported key range");
}

template <class T>
core::StateImage MakeImage(core::PartitionId partition,
                           std::shared_ptr<const T> payload) {
  return core::StateImage::Make<T>(Layout::kAccelerator, partition, 0,
                                   std::move(payload));
}

uint16_t InternName(std::vector<std::string> &names,
                    std::unordered_map<std::string, uint16_t> &index,
                    const std::string &name, std::size_t cap) {
  const auto it = index.find(name);
  if (it != index.end())
    return it->second;
  if (names.size() >= cap)
    throw std::runtime_error("TPC-H Q16: too many distinct brands/types");
  const auto id = static_cast<uint16_t>(names.size());
  names.push_back(name);
  index.emplace(name, id);
  return id;
}

void ComputeRanks(const std::vector<std::string> &names,
                  std::vector<uint16_t> &rank,
                  std::vector<uint16_t> &by_rank) {
  std::vector<uint16_t> order(names.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = static_cast<uint16_t>(i);
  std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
    return names[a] < names[b];
  });
  rank.assign(names.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i)
    rank[order[i]] = static_cast<uint16_t>(i);
  by_rank = std::move(order);
}

void RecomputeRanks(TpchQ16Meta &meta) {
  ComputeRanks(meta.brands, meta.brand_rank, meta.brand_by_rank);
  ComputeRanks(meta.types, meta.type_rank, meta.type_by_rank);
}

// ORDER BY supplier_cnt DESC, p_brand, p_type, p_size. New brands/types keep
// the relative order of the existing ones, so stored lists stay sorted.
struct RowLess {
  const TpchQ16Meta &meta;
  bool operator()(const Q16PrecomputedRow &a, const Q16PrecomputedRow &b) const {
    if (a.count != b.count)
      return a.count > b.count;
    if (a.brand_id != b.brand_id)
      return meta.brand_rank[a.brand_id] < meta.brand_rank[b.brand_id];
    if (a.type_id != b.type_id)
      return meta.type_rank[a.type_id] < meta.type_rank[b.type_id];
    return a.size < b.size;
  }
};

// ── Query arguments / merge ─────────────────────────────────────────────────
struct QueryParams {
  bool size_wanted[Layout::kMaxSize + 1] = {};
  std::string brand;
  std::string type_pattern;
};

bool ParseArguments(const core::QueryArguments &arguments, QueryParams &out) {
  if (arguments.values.size() < 3)
    return false;
  for (std::size_t i = 2; i < arguments.values.size() && i < 10; ++i) {
    const auto &value = arguments.values[i];
    if (value.is_null)
      continue;
    int64_t size = 0;
    if (std::holds_alternative<int64_t>(value.payload))
      size = std::get<int64_t>(value.payload);
    else if (std::holds_alternative<uint64_t>(value.payload))
      size = static_cast<int64_t>(std::get<uint64_t>(value.payload));
    else
      continue;
    if (size >= Layout::kMinSize && size <= Layout::kMaxSize)
      out.size_wanted[static_cast<std::size_t>(size)] = true;
  }
  if (!arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    out.brand = std::get<std::string>(arguments.values[0].payload);
  if (!arguments.values[1].is_null &&
      std::holds_alternative<std::string>(arguments.values[1].payload))
    out.type_pattern = std::get<std::string>(arguments.values[1].payload);
  return true;
}

// k-way merge of the requested per-size lists (each already sorted), applying
// the brand / type filters. Cost is proportional to the rows scanned in the
// requested sizes only.
void EmitMergedSlow(const TpchQ16Meta &meta,
                const std::vector<const TpchQ16SizeList *> &lists,
                const QueryParams &params, core::AuxRowSink &sink) {
  int32_t exclude_id = -1;
  if (!params.brand.empty()) {
    const auto it = meta.brand_index.find(params.brand);
    if (it != meta.brand_index.end())
      exclude_id = static_cast<int32_t>(it->second);
  }
  std::vector<char> type_ok(meta.types.size(), 1);
  if (!params.type_pattern.empty())
    for (std::size_t tid = 0; tid < meta.types.size(); ++tid)
      type_ok[tid] = TpchQ16TypeNotLike(meta.types[tid], params.type_pattern);

  struct Cursor {
    const Q16PrecomputedRow *cur;
    const Q16PrecomputedRow *end;
  };
  const auto accept = [&](const Q16PrecomputedRow &r) {
    if (exclude_id >= 0 && static_cast<int32_t>(r.brand_id) == exclude_id)
      return false;
    return type_ok[r.type_id] != 0;
  };
  const auto skip = [&](Cursor &c) {
    while (c.cur != c.end && !accept(*c.cur))
      ++c.cur;
  };
  std::vector<Cursor> cursors;
  for (const auto *list : lists) {
    if (!list || list->rows.empty())
      continue;
    Cursor c{list->rows.data(), list->rows.data() + list->rows.size()};
    skip(c);
    if (c.cur != c.end)
      cursors.push_back(c);
  }

  CustomTimer t_emit(CustomProfilingEnabled());
  sink.Begin({{{"p_brand", core::AuxType::String, false},
               {"p_type", core::AuxType::String, false},
               {"p_size", core::AuxType::Int64, false},
               {"supplier_cnt", core::AuxType::Int64, false}}});
  core::AuxRow row;
  row.values.resize(4);
  row.values[0].type = core::AuxType::String;
  row.values[0].is_null = false;
  row.values[0].payload = std::string();
  row.values[1].type = core::AuxType::String;
  row.values[1].is_null = false;
  row.values[1].payload = std::string();
  row.values[2].type = core::AuxType::Int64;
  row.values[2].is_null = false;
  row.values[2].payload = int64_t{0};
  row.values[3].type = core::AuxType::Int64;
  row.values[3].is_null = false;
  row.values[3].payload = int64_t{0};
  const RowLess less{meta};
  int64_t emitted = 0;
  for (;;) {
    Cursor *best = nullptr;
    for (auto &c : cursors)
      if (c.cur != c.end && (!best || less(*c.cur, *best->cur)))
        best = &c;
    if (!best)
      break;
    const auto &r = *best->cur;
    std::get<std::string>(row.values[0].payload) = meta.brands[r.brand_id];
    std::get<std::string>(row.values[1].payload) = meta.types[r.type_id];
    std::get<int64_t>(row.values[2].payload) = static_cast<int64_t>(r.size);
    std::get<int64_t>(row.values[3].payload) = r.count;
    sink.WriteRow(row);
    ++emitted;
    ++best->cur;
    skip(*best);
  }
  EmitCustomProfile("merge_emit", t_emit.us(), 0, emitted, 0);
  sink.End();
}


// Fast path. Every surviving row becomes one self-describing 64-bit sort key
//   (INT32_MAX - count) << 30 | brand_rank << 22 | type_rank << 6 | size
// (31 + 8 + 16 + 6 bits, ascending key == ORDER BY count DESC, brand, type,
// size). Each per-size list is already sorted, so its keys form a sorted run;
// the runs are merged pairwise into one reusable buffer (no allocation after
// warm-up) and decoded straight into the output row.
// Branch-free two-run merge (keys are unique, so ties never occur).
inline void MergeBranchless(const uint64_t *a, const uint64_t *a_end,
                            const uint64_t *b, const uint64_t *b_end,
                            uint64_t *out) {
  while (a != a_end && b != b_end) {
    const uint64_t x = *a, y = *b;
    const bool take_b = y < x;
    *out++ = take_b ? y : x;
    b += take_b;
    a += !take_b;
  }
  while (a != a_end)
    *out++ = *a++;
  while (b != b_end)
    *out++ = *b++;
}

constexpr int64_t kFastMaxCount = 0x7FFFFFFF;

void EmitMerged(const TpchQ16Meta &meta,
                const std::vector<const TpchQ16SizeList *> &lists,
                const QueryParams &params, core::AuxRowSink &sink) {
  bool fast = meta.brands.size() <= 256 && meta.types.size() <= 0x10000;
  std::size_t total = 0;
  for (const auto *list : lists)
    if (list)
      total += list->rows.size();
  if (!fast) {
    EmitMergedSlow(meta, lists, params, sink);
    return;
  }
  CustomTimer t_merge(CustomProfilingEnabled());
  uint32_t exclude_rank = 0xFFFFFFFFu;
  if (!params.brand.empty()) {
    const auto it = meta.brand_index.find(params.brand);
    if (it != meta.brand_index.end())
      exclude_rank = meta.brand_rank[it->second];
  }
  thread_local std::vector<uint8_t> type_ok;
  thread_local std::vector<uint64_t> buf_a, buf_b;
  type_ok.assign(meta.types.size(), 1);
  if (!params.type_pattern.empty()) {
    const std::string &pat = params.type_pattern;
    // "literal%" (the TPC-H shape) is a plain prefix test.
    const bool prefix_only =
        pat.size() >= 1 && pat.back() == '%' &&
        pat.find_first_of("%_") == pat.size() - 1;
    for (std::size_t tid = 0; tid < meta.types.size(); ++tid) {
      if (prefix_only) {
        const std::string &t = meta.types[tid];
        const std::size_t plen = pat.size() - 1;
        type_ok[tid] = !(t.size() >= plen && std::memcmp(t.data(), pat.data(), plen) == 0);
      } else {
        type_ok[tid] = TpchQ16TypeNotLike(meta.types[tid], pat);
      }
    }
  }
  buf_a.resize(total);
  buf_b.resize(total);

  const uint16_t *brand_rank = meta.brand_rank.data();
  const uint16_t *type_rank = meta.type_rank.data();
  const uint8_t *ok = type_ok.data();
  // Filter + key each list into buf_a as consecutive sorted runs.
  uint64_t *out = buf_a.data();
  std::size_t run_bounds[64];
  std::size_t nruns = 0;
  run_bounds[0] = 0;
  for (const auto *list : lists) {
    if (!list || list->rows.empty() || nruns + 1 >= 63)
      continue;
    const Q16PrecomputedRow *r = list->rows.data();
    const std::size_t n = list->rows.size();
    uint64_t *o = out;
    for (std::size_t i = 0; i < n; ++i) {
      const uint32_t br = brand_rank[r[i].brand_id];
      const uint32_t tid = r[i].type_id;
      const uint64_t key =
          (static_cast<uint64_t>(kFastMaxCount - r[i].count) << 30) |
          (static_cast<uint64_t>(br) << 22) |
          (static_cast<uint64_t>(type_rank[tid]) << 6) |
          static_cast<uint64_t>(r[i].size);
      *o = key;
      o += (br != exclude_rank) & (ok[tid] != 0);
    }
    out = o;
    run_bounds[++nruns] = static_cast<std::size_t>(out - buf_a.data());
  }
  // Bottom-up pairwise merge of runs, ping-ponging between the buffers.
  uint64_t *src = buf_a.data();
  uint64_t *dst = buf_b.data();
  while (nruns > 1) {
    std::size_t next_runs = 0;
    std::size_t next_bounds[64];
    next_bounds[0] = 0;
    for (std::size_t i = 0; i < nruns; i += 2) {
      const std::size_t lo = run_bounds[i];
      const std::size_t mid = run_bounds[i + 1];
      const std::size_t hi = (i + 1 < nruns) ? run_bounds[i + 2] : mid;
      MergeBranchless(src + lo, src + mid, src + mid, src + hi, dst + lo);
      next_bounds[++next_runs] = hi;
    }
    std::copy(next_bounds, next_bounds + next_runs + 1, run_bounds);
    nruns = next_runs;
    std::swap(src, dst);
  }
  const std::size_t count = nruns ? run_bounds[1] : 0;
  const double merge_us = t_merge.us();

  CustomTimer t_emit(CustomProfilingEnabled());
  sink.Begin({{{"p_brand", core::AuxType::String, false},
               {"p_type", core::AuxType::String, false},
               {"p_size", core::AuxType::Int64, false},
               {"supplier_cnt", core::AuxType::Int64, false}}});
  core::AuxRow row;
  row.values.resize(4);
  row.values[0].type = core::AuxType::String;
  row.values[0].is_null = false;
  row.values[0].payload = std::string();
  row.values[1].type = core::AuxType::String;
  row.values[1].is_null = false;
  row.values[1].payload = std::string();
  row.values[2].type = core::AuxType::Int64;
  row.values[2].is_null = false;
  row.values[2].payload = int64_t{0};
  row.values[3].type = core::AuxType::Int64;
  row.values[3].is_null = false;
  row.values[3].payload = int64_t{0};
  auto &brand_cell = std::get<std::string>(row.values[0].payload);
  auto &type_cell = std::get<std::string>(row.values[1].payload);
  auto &size_cell = std::get<int64_t>(row.values[2].payload);
  auto &count_cell = std::get<int64_t>(row.values[3].payload);
  const std::string *brand_names = meta.brands.data();
  const std::string *type_names = meta.types.data();
  const uint16_t *brand_by_rank = meta.brand_by_rank.data();
  const uint16_t *type_by_rank = meta.type_by_rank.data();
  uint32_t last_brand = 0xFFFFFFFFu, last_type = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < count; ++i) {
    const uint64_t key = src[i];
    const uint32_t br = static_cast<uint32_t>((key >> 22) & 0xFF);
    const uint32_t tr = static_cast<uint32_t>((key >> 6) & 0xFFFF);
    if (br != last_brand) {
      brand_cell = brand_names[brand_by_rank[br]];
      last_brand = br;
    }
    if (tr != last_type) {
      type_cell = type_names[type_by_rank[tr]];
      last_type = tr;
    }
    size_cell = static_cast<int64_t>(key & 0x3F);
    count_cell = kFastMaxCount - static_cast<int64_t>(key >> 30);
    sink.WriteRow(row);
  }
  EmitCustomProfile("merge_only", merge_us, 0, static_cast<int64_t>(count), 0);
  EmitCustomProfile("emit_only", t_emit.us(), 0, static_cast<int64_t>(count), 0);
  sink.End();
}

// ── Incremental maintenance (copy-on-write overlay over published pages) ────
class Stager {
public:
  explicit Stager(core::AuxEngineServices &services)
      : services(services), visibility(services.CurrentVisibility()) {}

  bool Open() {
    meta_pub = Fetch<TpchQ16Meta>(Layout::kMeta);
    return meta_pub != nullptr;
  }

  void Apply(const TpchQ16Op &op) {
    switch (op.kind) {
    case TpchQ16Op::Kind::PartInsert:
      PartInsert(op);
      break;
    case TpchQ16Op::Kind::PartDelete:
      PartDelete(op.key);
      break;
    case TpchQ16Op::Kind::PartsuppInsert:
      PartsuppInsert(op.key, op.key2);
      break;
    case TpchQ16Op::Kind::PartsuppDelete:
      PartsuppDelete(op.key, op.key2);
      break;
    case TpchQ16Op::Kind::SupplierSet:
      SupplierSet(op.key, op.eligible);
      break;
    }
  }

  // Re-sorts the per-size lists for every group whose count changed and
  // appends all modified pages to `out`.
  void Finish(std::vector<core::StateImage> &out) {
    for (const auto &[code, before] : touched) {
      const auto *gm = PeekGroup(code);
      const int32_t after = gm ? gm->eligible_count : 0;
      if (after == before)
        continue;
      const int32_t size = CodeSize(code);
      auto &list = Mut<TpchQ16SizeList>(
          lists, Layout::SizePartition(size),
          [] { return std::make_shared<TpchQ16SizeList>(); });
      const RowLess less{Meta()};
      const auto brand = static_cast<uint16_t>(CodeBrand(code));
      const auto type = CodeType(code);
      if (before > 0) {
        const Q16PrecomputedRow old_row{brand, type, size, before};
        auto it = std::lower_bound(list.rows.begin(), list.rows.end(), old_row,
                                   less);
        if (it == list.rows.end() || it->brand_id != brand ||
            it->type_id != type) {
          it = std::find_if(list.rows.begin(), list.rows.end(),
                            [&](const Q16PrecomputedRow &r) {
                              return r.brand_id == brand && r.type_id == type;
                            });
        }
        if (it != list.rows.end())
          list.rows.erase(it);
      }
      if (after > 0) {
        const Q16PrecomputedRow new_row{brand, type, size, after};
        list.rows.insert(std::lower_bound(list.rows.begin(), list.rows.end(),
                                          new_row, less),
                         new_row);
      }
    }
    if (meta_mut)
      out.push_back(MakeImage<TpchQ16Meta>(
          Layout::kMeta, std::shared_ptr<const TpchQ16Meta>(std::move(meta_mut))));
    Collect(lists, out);
    Collect(groups, out);
    Collect(parts, out);
    Collect(ps, out);
    Collect(supp, out);
  }

private:
  template <class T> struct PageSet {
    struct Entry {
      std::shared_ptr<const T> published;
      std::shared_ptr<T> mutated;
    };
    std::unordered_map<core::PartitionId, Entry> entries;
  };

  template <class T>
  std::shared_ptr<const T> Fetch(core::PartitionId id) {
    const auto snapshot = services.StateStore().ReadPartitions(visibility, {id});
    for (const auto &image : snapshot.partitions)
      if (image.partition == id)
        return std::static_pointer_cast<const T>(image.payload);
    return nullptr;
  }

  template <class T> typename PageSet<T>::Entry &EntryOf(PageSet<T> &set,
                                                       core::PartitionId id) {
    auto it = set.entries.find(id);
    if (it == set.entries.end()) {
      typename PageSet<T>::Entry entry;
      entry.published = Fetch<T>(id);
      it = set.entries.emplace(id, std::move(entry)).first;
    }
    return it->second;
  }

  template <class T> const T *Peek(PageSet<T> &set, core::PartitionId id) {
    auto &entry = EntryOf(set, id);
    return entry.mutated ? entry.mutated.get() : entry.published.get();
  }

  template <class T, class Init>
  T &Mut(PageSet<T> &set, core::PartitionId id, Init init) {
    auto &entry = EntryOf(set, id);
    if (!entry.mutated)
      entry.mutated = entry.published ? std::make_shared<T>(*entry.published)
                                      : init();
    return *entry.mutated;
  }

  template <class T>
  void Collect(PageSet<T> &set, std::vector<core::StateImage> &out) {
    for (auto &[id, entry] : set.entries)
      if (entry.mutated)
        out.push_back(MakeImage<T>(
            id, std::shared_ptr<const T>(std::move(entry.mutated))));
  }

  const TpchQ16Meta &Meta() const { return meta_mut ? *meta_mut : *meta_pub; }

  TpchQ16Meta &MutableMeta() {
    if (!meta_mut)
      meta_mut = std::make_shared<TpchQ16Meta>(*meta_pub);
    return *meta_mut;
  }

  uint16_t Brand(const std::string &name) {
    const auto &m = Meta();
    const auto it = m.brand_index.find(name);
    if (it != m.brand_index.end())
      return it->second;
    auto &mm = MutableMeta();
    const auto id = InternName(mm.brands, mm.brand_index, name, Layout::kBrandCap);
    RecomputeRanks(mm);
    return id;
  }

  uint16_t Type(const std::string &name) {
    const auto &m = Meta();
    const auto it = m.type_index.find(name);
    if (it != m.type_index.end())
      return it->second;
    auto &mm = MutableMeta();
    const auto id = InternName(mm.types, mm.type_index, name, 0xFFFF);
    RecomputeRanks(mm);
    return id;
  }

  // ── pages ────────────────────────────────────────────────────────────────
  TpchQ16PartInfo InfoOf(int64_t partkey) {
    const auto *page = Peek(parts, Layout::PartPartition(partkey));
    if (!page)
      return {};
    return page->info[static_cast<std::size_t>(partkey) & (kPartPageSize - 1)];
  }

  void SetInfo(int64_t partkey, const TpchQ16PartInfo &info) {
    auto &page = Mut<TpchQ16PartPage>(parts, Layout::PartPartition(partkey), [] {
      auto p = std::make_shared<TpchQ16PartPage>();
      p->Init();
      return p;
    });
    page.info[static_cast<std::size_t>(partkey) & (kPartPageSize - 1)] = info;
  }

  bool Eligible(int64_t suppkey) {
    const auto *page = Peek(supp, Layout::SuppPartition(suppkey));
    if (!page)
      return true;
    return page->eligible[static_cast<std::size_t>(suppkey) &
                          (kSuppPageSize - 1)] != 0;
  }

  TpchQ16SupplierPage &MutSupp(int64_t suppkey) {
    return Mut<TpchQ16SupplierPage>(supp, Layout::SuppPartition(suppkey), [] {
      auto p = std::make_shared<TpchQ16SupplierPage>();
      p->Init();
      return p;
    });
  }

  TpchQ16PartsuppPage &MutPs(int64_t partkey) {
    return Mut<TpchQ16PartsuppPage>(ps, Layout::PsPartition(partkey), [] {
      auto p = std::make_shared<TpchQ16PartsuppPage>();
      p->Init();
      return p;
    });
  }

  const TpchQ16GroupMembers *PeekGroup(uint32_t code) {
    const auto *page = Peek(
        groups, Layout::GroupPartition(CodeSize(code), CodeBrand(code)));
    if (!page)
      return nullptr;
    const auto type = CodeType(code);
    const auto it = std::lower_bound(
        page->groups.begin(), page->groups.end(), type,
        [](const TpchQ16GroupMembers &g, uint16_t t) { return g.type_id < t; });
    return it != page->groups.end() && it->type_id == type ? &*it : nullptr;
  }

  TpchQ16GroupMembers *FindGroup(uint32_t code, bool create) {
    if (!create && !PeekGroup(code))
      return nullptr;
    auto &page = Mut<TpchQ16GroupPage>(
        groups, Layout::GroupPartition(CodeSize(code), CodeBrand(code)),
        [] { return std::make_shared<TpchQ16GroupPage>(); });
    const auto type = CodeType(code);
    auto it = std::lower_bound(
        page.groups.begin(), page.groups.end(), type,
        [](const TpchQ16GroupMembers &g, uint16_t t) { return g.type_id < t; });
    if (it == page.groups.end() || it->type_id != type) {
      TpchQ16GroupMembers fresh;
      fresh.type_id = type;
      it = page.groups.insert(it, std::move(fresh));
    }
    return &*it;
  }

  void Touch(uint32_t code, const TpchQ16GroupMembers &gm) {
    touched.emplace(code, gm.eligible_count);
  }

  // One more (delta > 0) or one fewer partsupp row of `suppkey` in the group.
  void GroupAdd(uint32_t code, int64_t suppkey, int delta) {
    auto *gm = FindGroup(code, delta > 0);
    if (!gm)
      return;
    const auto pos =
        std::lower_bound(gm->suppkeys.begin(), gm->suppkeys.end(), suppkey);
    const auto idx = static_cast<std::ptrdiff_t>(pos - gm->suppkeys.begin());
    const bool found = pos != gm->suppkeys.end() && *pos == suppkey;
    if (delta > 0) {
      Touch(code, *gm);
      if (found) {
        ++gm->ref_counts[static_cast<std::size_t>(idx)];
      } else {
        gm->suppkeys.insert(pos, suppkey);
        gm->ref_counts.insert(gm->ref_counts.begin() + idx, 1);
        auto &sp = MutSupp(suppkey);
        sp.groups.Insert(static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1),
                         code);
        if (Eligible(suppkey))
          ++gm->eligible_count;
      }
    } else {
      if (!found)
        return;
      Touch(code, *gm);
      if (--gm->ref_counts[static_cast<std::size_t>(idx)] == 0) {
        gm->suppkeys.erase(gm->suppkeys.begin() + idx);
        gm->ref_counts.erase(gm->ref_counts.begin() + idx);
        auto &sp = MutSupp(suppkey);
        sp.groups.Erase(static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1),
                        code);
        if (Eligible(suppkey))
          --gm->eligible_count;
      }
    }
  }

  static bool InGroup(const TpchQ16PartInfo &info) {
    return info.brand_id != kNoBrand && SizeInRange(info.size);
  }
  static uint32_t CodeOf(const TpchQ16PartInfo &info) {
    return GroupCode(info.size, info.brand_id, info.type_id);
  }

  // ── ops ──────────────────────────────────────────────────────────────────
  void RemovePart(int64_t partkey, const TpchQ16PartInfo &info) {
    if (InGroup(info)) {
      if (const auto *page = Peek(ps, Layout::PsPartition(partkey))) {
        const auto row = static_cast<std::size_t>(partkey) & (kPsPageSize - 1);
        const auto code = CodeOf(info);
        for (const auto *p = page->rows.Begin(row); p != page->rows.End(row); ++p)
          GroupAdd(code, *p, -1);
      }
    }
    SetInfo(partkey, TpchQ16PartInfo{});
  }

  void PartInsert(const TpchQ16Op &op) {
    CheckKey(op.key, "partkey");
    const auto current = InfoOf(op.key);
    if (current.brand_id != kNoBrand)
      RemovePart(op.key, current);
    TpchQ16PartInfo info;
    info.brand_id = Brand(op.brand);
    info.type_id = Type(op.type);
    info.size = op.size;
    SetInfo(op.key, info);
    if (!InGroup(info))
      return;
    if (const auto *page = Peek(ps, Layout::PsPartition(op.key))) {
      const auto row = static_cast<std::size_t>(op.key) & (kPsPageSize - 1);
      const auto code = CodeOf(info);
      for (const auto *p = page->rows.Begin(row); p != page->rows.End(row); ++p)
        GroupAdd(code, *p, +1);
    }
  }

  void PartDelete(int64_t partkey) {
    if (!Layout::KeyInRange(partkey))
      return;
    const auto current = InfoOf(partkey);
    if (current.brand_id == kNoBrand)
      return;
    RemovePart(partkey, current);
  }

  void PartsuppInsert(int64_t partkey, int64_t suppkey) {
    CheckKey(partkey, "partkey");
    CheckKey(suppkey, "suppkey");
    MutPs(partkey).rows.Insert(
        static_cast<std::size_t>(partkey) & (kPsPageSize - 1), suppkey);
    const auto info = InfoOf(partkey);
    if (InGroup(info))
      GroupAdd(CodeOf(info), suppkey, +1);
  }

  void PartsuppDelete(int64_t partkey, int64_t suppkey) {
    if (!Layout::KeyInRange(partkey) || !Layout::KeyInRange(suppkey))
      return;
    const auto *page = Peek(ps, Layout::PsPartition(partkey));
    if (!page)
      return;
    const auto row = static_cast<std::size_t>(partkey) & (kPsPageSize - 1);
    if (std::find(page->rows.Begin(row), page->rows.End(row), suppkey) ==
        page->rows.End(row))
      return;
    MutPs(partkey).rows.Erase(row, suppkey);
    const auto info = InfoOf(partkey);
    if (InGroup(info))
      GroupAdd(CodeOf(info), suppkey, -1);
  }

  void SupplierSet(int64_t suppkey, bool eligible) {
    CheckKey(suppkey, "suppkey");
    if (Eligible(suppkey) == eligible)
      return;
    auto &page = MutSupp(suppkey);
    const auto row = static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1);
    page.eligible[row] = eligible ? 1 : 0;
    for (const auto *p = page.groups.Begin(row); p != page.groups.End(row); ++p) {
      auto *gm = FindGroup(*p, false);
      if (!gm)
        continue;
      Touch(*p, *gm);
      gm->eligible_count += eligible ? 1 : -1;
    }
  }

  core::AuxEngineServices &services;
  core::VisibilityToken visibility;
  std::shared_ptr<const TpchQ16Meta> meta_pub;
  std::shared_ptr<TpchQ16Meta> meta_mut;
  PageSet<TpchQ16SizeList> lists;
  PageSet<TpchQ16GroupPage> groups;
  PageSet<TpchQ16PartPage> parts;
  PageSet<TpchQ16PartsuppPage> ps;
  PageSet<TpchQ16SupplierPage> supp;
  // group code -> eligible count before this commit first changed it
  std::unordered_map<uint32_t, int32_t> touched;
};

template <class T, class V>
void BuildCsr(TpchQ16Csr<T> &csr, std::size_t rows,
              const std::vector<uint16_t> &local, const std::vector<V> &values) {
  csr.Init(rows);
  for (const auto l : local)
    ++csr.off[static_cast<std::size_t>(l) + 1];
  for (std::size_t i = 1; i < csr.off.size(); ++i)
    csr.off[i] += csr.off[i - 1];
  csr.data.resize(values.size());
  std::vector<uint32_t> cursor(csr.off.begin(), csr.off.end() - 1);
  for (std::size_t i = 0; i < local.size(); ++i)
    csr.data[cursor[local[i]]++] = static_cast<T>(values[i]);
}

} // namespace

// ── Public predicate helpers ────────────────────────────────────────────────
bool TpchQ16IsComplaintSupplier(const std::string &comment) {
  return IsComplaintComment(comment);
}

bool TpchQ16TypeNotLike(const std::string &type, const std::string &pattern) {
  return !SqlLike(type, pattern);
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ16TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ16TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  ops.clear();
}
bool TpchQ16TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ16Op> &TpchQ16TxnBuffer::Ops() const { return ops; }
void TpchQ16TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
}
void TpchQ16TxnBuffer::AddOp(TpchQ16Op op) {
  pending = true;
  ops.push_back(std::move(op));
}

// ── Module identity / dependencies ─────────────────────────────────────────
core::AcceleratorId TpchQ16Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ16Module::Dependencies() const {
  std::vector<core::TableDependency> deps;
  deps.push_back({config.part_table, {0, 1, 2, 3}, {0, 3, 4, 5}});
  deps.push_back({config.partsupp_table, {0, 1}, {0, 1}});
  deps.push_back({config.supplier_table, {0, 1}, {0, 6}});
  return deps;
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ16Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ16TxnBuffer>();
}

// ── Build ───────────────────────────────────────────────────────────────────
void TpchQ16Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services);
  for (auto &image : state.images)
    builder.Write(std::move(image));
  builder.Finish();
}

// ── Mutation interpretation ────────────────────────────────────────────────
core::InvalidationPlan TpchQ16Module::InterpretMutation(
    const core::MutationBatch &mutation) const {
  const bool partsupp = mutation.table == config.partsupp_table;
  const bool part = mutation.table == config.part_table;
  const bool supplier = mutation.table == config.supplier_table;

  if (!partsupp && !part && !supplier)
    return {};

  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;

  if (supplier) {
    if (mutation.kind != core::MutationKind::Update)
      return plan;
    for (const auto &col : mutation.updated_columns)
      if (col == 0 || col == 1)
        return plan;
    return {};
  }
  return plan;
}

void TpchQ16Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &services,
                                       core::AuxTxnBuffer &txn_buffer) {
  (void)services;
  auto &buffer = dynamic_cast<TpchQ16TxnBuffer &>(txn_buffer);
  const bool is_update = mutation.kind == core::MutationKind::Update;
  const bool is_delete = mutation.kind == core::MutationKind::Delete;

  const auto add_supplier = [&](int64_t suppkey, bool eligible) {
    TpchQ16Op op;
    op.kind = TpchQ16Op::Kind::SupplierSet;
    op.key = suppkey;
    op.eligible = eligible;
    buffer.AddOp(std::move(op));
  };
  const auto add_part_delete = [&](int64_t partkey) {
    TpchQ16Op op;
    op.kind = TpchQ16Op::Kind::PartDelete;
    op.key = partkey;
    buffer.AddOp(std::move(op));
  };
  const auto add_part_insert = [&](const TpchQ16PartInput &in) {
    TpchQ16Op op;
    op.kind = TpchQ16Op::Kind::PartInsert;
    op.key = in.partkey;
    op.size = in.size;
    op.brand = in.brand;
    op.type = in.type;
    buffer.AddOp(std::move(op));
  };
  const auto add_partsupp = [&](bool insert, int64_t partkey, int64_t suppkey) {
    TpchQ16Op op;
    op.kind = insert ? TpchQ16Op::Kind::PartsuppInsert
                     : TpchQ16Op::Kind::PartsuppDelete;
    op.key = partkey;
    op.key2 = suppkey;
    buffer.AddOp(std::move(op));
  };

  if (mutation.table == config.supplier_table) {
    if (is_update) {
      bool watched = false;
      for (const auto &col : mutation.updated_columns)
        if (col == 0 || col == 1)
          watched = true;
      if (!watched)
        return;
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) {
        TpchQ16SupplierInput old_in, new_in;
        if (!ReadSupplier(mutation.old_rows[i], old_in) ||
            !ReadSupplier(mutation.new_rows[i], new_in)) {
          buffer.MarkRebuild();
          return;
        }
        // The old key no longer has a supplier row: eligible (NOT IN).
        if (old_in.suppkey != new_in.suppkey)
          add_supplier(old_in.suppkey, true);
        add_supplier(new_in.suppkey, !TpchQ16IsComplaintSupplier(new_in.comment));
      }
      return;
    }
    const auto &rows = is_delete ? mutation.old_rows : mutation.new_rows;
    for (const auto &row : rows) {
      TpchQ16SupplierInput in;
      if (!ReadSupplier(row, in)) {
        buffer.MarkRebuild();
        return;
      }
      // Delete: the key has no supplier row any more, hence is eligible.
      add_supplier(in.suppkey,
                   is_delete || !TpchQ16IsComplaintSupplier(in.comment));
    }
    return;
  }

  if (mutation.table == config.part_table) {
    if (is_update) {
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) {
        TpchQ16PartInput old_in, new_in;
        if (!ReadPart(mutation.old_rows[i], old_in) ||
            !ReadPart(mutation.new_rows[i], new_in)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_in.partkey == new_in.partkey && old_in.brand == new_in.brand &&
            old_in.type == new_in.type && old_in.size == new_in.size)
          continue;
        add_part_delete(old_in.partkey);
        add_part_insert(new_in);
      }
      return;
    }
    const auto &rows = is_delete ? mutation.old_rows : mutation.new_rows;
    for (const auto &row : rows) {
      TpchQ16PartInput in;
      if (!ReadPart(row, in)) {
        buffer.MarkRebuild();
        return;
      }
      if (is_delete)
        add_part_delete(in.partkey);
      else
        add_part_insert(in);
    }
    return;
  }

  if (mutation.table == config.partsupp_table) {
    if (is_update) {
      bool watched = false;
      for (const auto &col : mutation.updated_columns)
        if (col == 0 || col == 1)
          watched = true;
      if (!watched)
        return;
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) {
        TpchQ16PartsuppInput old_in, new_in;
        if (!ReadPartsupp(mutation.old_rows[i], old_in) ||
            !ReadPartsupp(mutation.new_rows[i], new_in)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_in.partkey == new_in.partkey && old_in.suppkey == new_in.suppkey)
          continue;
        add_partsupp(false, old_in.partkey, old_in.suppkey);
        add_partsupp(true, new_in.partkey, new_in.suppkey);
      }
      return;
    }
    const auto &rows = is_delete ? mutation.old_rows : mutation.new_rows;
    for (const auto &row : rows) {
      TpchQ16PartsuppInput in;
      if (!ReadPartsupp(row, in)) {
        buffer.MarkRebuild();
        return;
      }
      add_partsupp(!is_delete, in.partkey, in.suppkey);
    }
    return;
  }
}

// ── Maintenance staging ────────────────────────────────────────────────────
core::StagedMaintenanceOutcome TpchQ16Module::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    core::AuxTxnBuffer &txn_buffer) {
  auto &buffer = dynamic_cast<TpchQ16TxnBuffer &>(txn_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (!buffer.HasPendingChanges() ||
      (!buffer.RequiresRebuild() && buffer.Ops().empty())) {
    outcome.action = core::MaintenanceAction::NoOp;
    return outcome;
  }

  const auto full_rebuild = [&]() {
    auto state = BuildState(services);
    outcome.updates = std::move(state.images);
    outcome.action = core::MaintenanceAction::RebuildAll;
    return outcome;
  };

  if (buffer.RequiresRebuild() || buffer.Ops().size() > kRebuildOpThreshold)
    return full_rebuild();

  // Apply the row deltas to copies of only the pages they touch; published
  // pages are never mutated (aborted commits leave no trace).
  Stager stager(services);
  if (!stager.Open())
    return full_rebuild();
  for (const auto &op : buffer.Ops())
    stager.Apply(op);
  stager.Finish(outcome.updates);
  outcome.action = outcome.updates.empty() ? core::MaintenanceAction::NoOp
                                           : core::MaintenanceAction::ApplyDelta;
  return outcome;
}

// ── Validity / read path ───────────────────────────────────────────────────
bool TpchQ16Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ16Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  QueryParams params;
  if (!ParseArguments(arguments, params)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Meta + only the requested sizes' lists.
  CustomTimer t_load(CustomProfilingEnabled());
  std::vector<core::PartitionId> wanted{Layout::kMeta};
  for (int32_t size = Layout::kMinSize; size <= Layout::kMaxSize; ++size)
    if (params.size_wanted[size])
      wanted.push_back(Layout::SizePartition(size));
  const auto snapshot =
      services.StateStore().ReadPartitions(services.CurrentVisibility(), wanted);
  const TpchQ16Meta *meta = nullptr;
  std::vector<const TpchQ16SizeList *> lists;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == Layout::kMeta)
      meta = image.As<TpchQ16Meta>();
    else
      lists.push_back(image.As<TpchQ16SizeList>());
  }
  if (!meta) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitCustomProfile("load_partitions", t_load.us(),
                    static_cast<int64_t>(lists.size()), 0, 0);
  EmitMerged(*meta, lists, params, sink);
}

namespace {

// Build partials. Partitions fold in table order, so ids and last-wins rows
// equal a serial scan's.
struct SupplierPartial {
  std::vector<std::unique_ptr<TpchQ16SupplierPage>> pages;

  TpchQ16SupplierPage &Page(int64_t suppkey) {
    const auto chunk = static_cast<std::size_t>(suppkey >> Layout::kSuppShift);
    if (chunk >= pages.size())
      pages.resize(chunk + 1);
    if (!pages[chunk]) {
      pages[chunk] = std::make_unique<TpchQ16SupplierPage>();
      pages[chunk]->Init();
    }
    return *pages[chunk];
  }
  void MergeFrom(SupplierPartial &&from) {
    if (pages.size() < from.pages.size())
      pages.resize(from.pages.size());
    for (std::size_t chunk = 0; chunk < from.pages.size(); ++chunk) {
      if (!from.pages[chunk])
        continue;
      if (!pages[chunk]) {
        pages[chunk] = std::move(from.pages[chunk]);
        continue;
      }
      for (std::size_t i = 0; i < kSuppPageSize; ++i)
        pages[chunk]->eligible[i] &= from.pages[chunk]->eligible[i];
    }
  }
};

struct PartPartial {
  TpchQ16Meta meta; // names in order of first appearance within the partition
  std::vector<std::unique_ptr<TpchQ16PartPage>> pages;

  TpchQ16PartPage &Page(int64_t partkey) {
    const auto chunk = static_cast<std::size_t>(partkey >> Layout::kPartShift);
    if (chunk >= pages.size())
      pages.resize(chunk + 1);
    if (!pages[chunk]) {
      pages[chunk] = std::make_unique<TpchQ16PartPage>();
      pages[chunk]->Init();
    }
    return *pages[chunk];
  }
  void MergeFrom(PartPartial &&from) {
    std::vector<uint16_t> brand_ids, type_ids;
    for (const auto &name : from.meta.brands)
      brand_ids.push_back(InternName(meta.brands, meta.brand_index, name, Layout::kBrandCap));
    for (const auto &name : from.meta.types)
      type_ids.push_back(InternName(meta.types, meta.type_index, name, 0xFFFF));
    if (pages.size() < from.pages.size())
      pages.resize(from.pages.size());
    for (std::size_t chunk = 0; chunk < from.pages.size(); ++chunk) {
      if (!from.pages[chunk])
        continue;
      for (auto &info : from.pages[chunk]->info)
        if (info.brand_id != kNoBrand) {
          info.brand_id = brand_ids[info.brand_id];
          info.type_id = type_ids[info.type_id];
        }
      if (!pages[chunk]) {
        pages[chunk] = std::move(from.pages[chunk]);
        continue;
      }
      for (std::size_t i = 0; i < kPartPageSize; ++i)
        if (from.pages[chunk]->info[i].brand_id != kNoBrand)
          pages[chunk]->info[i] = from.pages[chunk]->info[i];
    }
  }
};

// Partsupp rows per page and the (size, brand, type) groups of their parts.
struct PsTemp {
  std::vector<uint16_t> local;
  std::vector<int64_t> sk;
};
struct InvTemp {
  std::vector<uint16_t> local;
  std::vector<uint32_t> code;
};

struct PartsuppPartial {
  std::vector<PsTemp> ps_temp;
  std::vector<uint32_t> gindex; // group slot -> index in `groups`
  std::vector<TpchQ16GroupMembers> groups;

  explicit PartsuppPartial(std::size_t group_slots) : gindex(group_slots, kNoGroup) {}

  TpchQ16GroupMembers &Group(std::size_t slot, uint16_t type_id) {
    if (gindex[slot] == kNoGroup) {
      gindex[slot] = static_cast<uint32_t>(groups.size());
      groups.emplace_back();
      groups.back().type_id = type_id;
    }
    return groups[gindex[slot]];
  }
  void AddMember(std::size_t slot, uint16_t type_id, int64_t suppkey) {
    auto &gm = Group(slot, type_id);
    const auto pos = std::lower_bound(gm.suppkeys.begin(), gm.suppkeys.end(), suppkey);
    const auto idx = pos - gm.suppkeys.begin();
    if (pos != gm.suppkeys.end() && *pos == suppkey) {
      ++gm.ref_counts[static_cast<std::size_t>(idx)];
      return;
    }
    gm.suppkeys.insert(pos, suppkey);
    gm.ref_counts.insert(gm.ref_counts.begin() + idx, 1);
  }
  void MergeFrom(PartsuppPartial &&from) {
    if (ps_temp.size() < from.ps_temp.size())
      ps_temp.resize(from.ps_temp.size());
    for (std::size_t chunk = 0; chunk < from.ps_temp.size(); ++chunk) {
      auto &mine = ps_temp[chunk];
      auto &theirs = from.ps_temp[chunk];
      mine.local.insert(mine.local.end(), theirs.local.begin(), theirs.local.end());
      mine.sk.insert(mine.sk.end(), theirs.sk.begin(), theirs.sk.end());
    }
    for (std::size_t slot = 0; slot < from.gindex.size(); ++slot) {
      if (from.gindex[slot] == kNoGroup)
        continue;
      auto &theirs = from.groups[from.gindex[slot]];
      if (gindex[slot] == kNoGroup) {
        Group(slot, theirs.type_id) = std::move(theirs);
        continue;
      }
      auto &mine = groups[gindex[slot]];
      std::vector<int64_t> keys;
      std::vector<int32_t> refs;
      std::size_t i = 0, j = 0;
      while (i < mine.suppkeys.size() || j < theirs.suppkeys.size()) {
        const bool take_mine = j == theirs.suppkeys.size() ||
                               (i < mine.suppkeys.size() && mine.suppkeys[i] <= theirs.suppkeys[j]);
        const bool both = i < mine.suppkeys.size() && j < theirs.suppkeys.size() &&
                          mine.suppkeys[i] == theirs.suppkeys[j];
        if (both) {
          keys.push_back(mine.suppkeys[i]);
          refs.push_back(mine.ref_counts[i++] + theirs.ref_counts[j++]);
        } else if (take_mine) {
          keys.push_back(mine.suppkeys[i]);
          refs.push_back(mine.ref_counts[i++]);
        } else {
          keys.push_back(theirs.suppkeys[j]);
          refs.push_back(theirs.ref_counts[j++]);
        }
      }
      mine.suppkeys = std::move(keys);
      mine.ref_counts = std::move(refs);
    }
  }
};

} // namespace

// ── State build ────────────────────────────────────────────────────────────
TpchQ16State TpchQ16Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q16 requires a typed initial scan source");

  auto supplier_pages = core::PartitionedBuild<SupplierPartial>(
      services, [] { return SupplierPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanSuppliers(services, parts,
                                    [&](std::size_t p, const TpchQ16SupplierInput &input) {
                                      sink(p, TypedRowBatch<TpchQ16SupplierInput>{input});
                                    });
      },
      [](SupplierPartial &partial, const TpchQ16SupplierInput &input) {
        CheckKey(input.suppkey, "suppkey");
        partial.Page(input.suppkey)
            .eligible[static_cast<std::size_t>(input.suppkey) & (kSuppPageSize - 1)] =
            TpchQ16IsComplaintSupplier(input.comment) ? 0 : 1;
      },
      [](SupplierPartial &into, SupplierPartial &&from) { into.MergeFrom(std::move(from)); });
  auto &supp_pages = supplier_pages.pages;
  const auto eligible_at = [&](int64_t key) {
    const auto chunk = static_cast<std::size_t>(key >> Layout::kSuppShift);
    if (chunk >= supp_pages.size() || !supp_pages[chunk])
      return true;
    return supp_pages[chunk]->eligible[static_cast<std::size_t>(key) &
                                       (kSuppPageSize - 1)] != 0;
  };

  auto part_partial = core::PartitionedBuild<PartPartial>(
      services, [] { return PartPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanParts(services, parts,
                                [&](std::size_t p, const TpchQ16PartInput &input) {
                                  sink(p, TypedRowBatch<TpchQ16PartInput>{input});
                                });
      },
      [](PartPartial &partial, const TpchQ16PartInput &input) {
        CheckKey(input.partkey, "partkey");
        TpchQ16PartInfo info;
        info.brand_id = InternName(partial.meta.brands, partial.meta.brand_index, input.brand,
                                   Layout::kBrandCap);
        info.type_id =
            InternName(partial.meta.types, partial.meta.type_index, input.type, 0xFFFF);
        info.size = input.size;
        partial.Page(input.partkey).info[static_cast<std::size_t>(input.partkey) &
                                         (kPartPageSize - 1)] = info;
      },
      [](PartPartial &into, PartPartial &&from) { into.MergeFrom(std::move(from)); });
  auto meta = std::make_shared<TpchQ16Meta>(std::move(part_partial.meta));
  auto &part_pages = part_partial.pages;
  RecomputeRanks(*meta);

  const std::size_t nb = meta->brands.size();
  const std::size_t nt = meta->types.size();
  const std::size_t sizes = static_cast<std::size_t>(Layout::kMaxSize);
  if (nb * nt * sizes > (std::size_t{1} << 26))
    throw std::runtime_error("TPC-H Q16: too many brand/type combinations");
  const auto gslot = [&](int32_t size, uint16_t brand, uint16_t type) {
    return (static_cast<std::size_t>(size - 1) * nb + brand) * nt + type;
  };

  auto partsupp = core::PartitionedBuild<PartsuppPartial>(
      services, [&] { return PartsuppPartial(nb * nt * sizes); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanPartsupp(services, parts,
                                   [&](std::size_t p, const TpchQ16PartsuppInput &input) {
                                     sink(p, TypedRowBatch<TpchQ16PartsuppInput>{input});
                                   });
      },
      [&](PartsuppPartial &partial, const TpchQ16PartsuppInput &input) {
        CheckKey(input.partkey, "partkey");
        CheckKey(input.suppkey, "suppkey");
        const auto pchunk = static_cast<std::size_t>(input.partkey >> Layout::kPsShift);
        if (pchunk >= partial.ps_temp.size())
          partial.ps_temp.resize(pchunk + 1);
        partial.ps_temp[pchunk].local.push_back(static_cast<uint16_t>(
            static_cast<std::size_t>(input.partkey) & (kPsPageSize - 1)));
        partial.ps_temp[pchunk].sk.push_back(input.suppkey);

        const auto part_chunk =
            static_cast<std::size_t>(input.partkey >> Layout::kPartShift);
        if (part_chunk >= part_pages.size() || !part_pages[part_chunk])
          return;
        const auto &info =
            part_pages[part_chunk]->info[static_cast<std::size_t>(input.partkey) &
                                         (kPartPageSize - 1)];
        if (info.brand_id == kNoBrand || !SizeInRange(info.size))
          return;
        partial.AddMember(gslot(info.size, info.brand_id, info.type_id), info.type_id,
                          input.suppkey);
      },
      [](PartsuppPartial &into, PartsuppPartial &&from) { into.MergeFrom(std::move(from)); });
  auto &ps_temp = partsupp.ps_temp;
  auto &gindex = partsupp.gindex;
  auto &built_groups = partsupp.groups;

  // Eligible suppliers per group, and the groups each supplier belongs to.
  std::vector<InvTemp> inv_temp;
  core::ParallelSlices(services, built_groups.size(), kGroupsGrain,
                       [&](std::size_t, std::size_t begin, std::size_t end) {
                         for (std::size_t g = begin; g < end; ++g) {
                           auto &gm = built_groups[g];
                           gm.eligible_count = static_cast<int32_t>(std::count_if(
                               gm.suppkeys.begin(), gm.suppkeys.end(), eligible_at));
                         }
                       });
  for (std::size_t slot = 0; slot < gindex.size(); ++slot) {
    if (gindex[slot] == kNoGroup)
      continue;
    const auto size = static_cast<int32_t>(slot / (nb * nt)) + 1;
    const auto brand = static_cast<uint16_t>(slot / nt % nb);
    const auto type = static_cast<uint16_t>(slot % nt);
    const auto code = GroupCode(size, brand, type);
    for (const auto suppkey : built_groups[gindex[slot]].suppkeys) {
      const auto schunk = static_cast<std::size_t>(suppkey >> Layout::kSuppShift);
      if (schunk >= inv_temp.size())
        inv_temp.resize(schunk + 1);
      inv_temp[schunk].local.push_back(static_cast<uint16_t>(
          static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1)));
      inv_temp[schunk].code.push_back(code);
    }
  }

  TpchQ16State state;
  auto &images = state.images;

  // Group pages and per-size sorted result lists.
  std::vector<TpchQ16SizeList> lists(sizes + 1);
  CustomTimer t_precompute(CustomProfilingEnabled());
  int64_t result_rows = 0;
  for (int32_t size = Layout::kMinSize; size <= Layout::kMaxSize; ++size) {
    for (std::size_t brand = 0; brand < nb; ++brand) {
      std::shared_ptr<TpchQ16GroupPage> page;
      for (std::size_t type = 0; type < nt; ++type) {
        const auto slot = gindex[gslot(size, static_cast<uint16_t>(brand),
                                       static_cast<uint16_t>(type))];
        if (slot == kNoGroup)
          continue;
        if (!page)
          page = std::make_shared<TpchQ16GroupPage>();
        auto &gm = built_groups[slot];
        if (gm.eligible_count > 0) {
          lists[static_cast<std::size_t>(size)].rows.push_back(
              {static_cast<uint16_t>(brand), static_cast<uint16_t>(type), size,
               gm.eligible_count});
          ++result_rows;
        }
        page->groups.push_back(std::move(gm));
      }
      if (page)
        images.push_back(MakeImage<TpchQ16GroupPage>(
            Layout::GroupPartition(size, static_cast<uint32_t>(brand)),
            std::shared_ptr<const TpchQ16GroupPage>(std::move(page))));
    }
  }
  gindex = {};
  built_groups = {};
  const RowLess less{*meta};
  for (int32_t size = Layout::kMinSize; size <= Layout::kMaxSize; ++size) {
    auto &rows = lists[static_cast<std::size_t>(size)].rows;
    std::sort(rows.begin(), rows.end(), less);
    images.push_back(MakeImage<TpchQ16SizeList>(
        Layout::SizePartition(size),
        std::make_shared<const TpchQ16SizeList>(
            std::move(lists[static_cast<std::size_t>(size)]))));
  }
  EmitCustomProfile("precompute", t_precompute.us(), 0, result_rows, 0);

  // Partsupp pages: CSR per chunk in parallel, images added in chunk order.
  std::vector<std::shared_ptr<const TpchQ16PartsuppPage>> ps_pages(ps_temp.size());
  core::ParallelSlices(
      services, ps_temp.size(), kPsPagesGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t chunk = begin; chunk < end; ++chunk) {
          auto &temp = ps_temp[chunk];
          if (temp.local.empty())
            continue;
          auto page = std::make_shared<TpchQ16PartsuppPage>();
          BuildCsr(page->rows, kPsPageSize, temp.local, temp.sk);
          temp = PsTemp{};
          ps_pages[chunk] = std::move(page);
        }
      });
  for (std::size_t chunk = 0; chunk < ps_pages.size(); ++chunk)
    if (ps_pages[chunk])
      images.push_back(MakeImage<TpchQ16PartsuppPage>(
          Layout::kPsBase + static_cast<core::PartitionId>(chunk),
          std::move(ps_pages[chunk])));
  ps_temp = {};

  // Supplier pages (eligibility + groups each supplier belongs to).
  const std::size_t supp_chunks = std::max(supp_pages.size(), inv_temp.size());
  std::vector<std::shared_ptr<const TpchQ16SupplierPage>> supp_images(supp_chunks);
  core::ParallelSlices(
      services, supp_chunks, kSuppPagesGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t chunk = begin; chunk < end; ++chunk) {
          const bool has_page = chunk < supp_pages.size() && supp_pages[chunk];
          const bool has_inv =
              chunk < inv_temp.size() && !inv_temp[chunk].local.empty();
          if (!has_page && !has_inv)
            continue;
          std::shared_ptr<TpchQ16SupplierPage> page;
          if (has_page) {
            page = std::shared_ptr<TpchQ16SupplierPage>(std::move(supp_pages[chunk]));
          } else {
            page = std::make_shared<TpchQ16SupplierPage>();
            page->Init();
          }
          if (has_inv) {
            BuildCsr(page->groups, kSuppPageSize, inv_temp[chunk].local,
                     inv_temp[chunk].code);
            inv_temp[chunk] = InvTemp{};
          }
          supp_images[chunk] = std::move(page);
        }
      });
  for (std::size_t chunk = 0; chunk < supp_images.size(); ++chunk)
    if (supp_images[chunk])
      images.push_back(MakeImage<TpchQ16SupplierPage>(
          Layout::kSuppBase + static_cast<core::PartitionId>(chunk),
          std::move(supp_images[chunk])));
  inv_temp = {};

  // Part pages and metadata.
  for (std::size_t chunk = 0; chunk < part_pages.size(); ++chunk) {
    if (!part_pages[chunk])
      continue;
    images.push_back(MakeImage<TpchQ16PartPage>(
        Layout::kPartBase + static_cast<core::PartitionId>(chunk),
        std::shared_ptr<const TpchQ16PartPage>(std::move(part_pages[chunk]))));
  }
  images.push_back(MakeImage<TpchQ16Meta>(
      Layout::kMeta, std::shared_ptr<const TpchQ16Meta>(std::move(meta))));
  return state;
}

std::vector<core::CalibrationWrite> TpchQ16Module::CalibrationWrites() const {
  return TpchQ16CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
