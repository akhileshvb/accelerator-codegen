#include "tpch/q16/duckdb/aux/module.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q16/calibration_writes.hpp"
#include "tpch/q16/duckdb/aux/stager.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

using namespace q16_pages;

constexpr uint32_t kNoGroup = 0xFFFFFFFFu;
// More row deltas than this in one commit: a full rebuild is cheaper.
constexpr std::size_t kRebuildOpThreshold = 200000;
// Ops per slice before a commit is applied as a batch.
constexpr std::size_t kOpGrain = 1024;
// Partsupp pages per slice: a page is ~20 us of CSR work.
constexpr std::size_t kPsPageGrain = 16;
// Supplier pages per slice: a page is a few us.
constexpr std::size_t kSuppPageGrain = 64;
// Groups per slice: counting a group's eligible suppliers is ~1 us per 50 members.
constexpr std::size_t kGroupGrain = 4096;
// Distinct (size, brand, type) slots the build is willing to index.
constexpr std::size_t kMaxGroupSlots = std::size_t{1} << 26;

bool IsComplaintComment(const std::string &comment) {
  return comment.find("Customer") != std::string::npos &&
         comment.find("Complaints") != std::string::npos;
}

// ── Row readers ─────────────────────────────────────────────────────────────
bool ReadSupplier(const core::AuxRow &row, TpchQ16SupplierInput &out) {
  int64_t suppkey = 0;
  if (!core::IntCell(row, 0, suppkey) || !core::StringCell(row, 1, out.comment))
    return false;
  out.suppkey = suppkey;
  return true;
}

bool ReadPart(const core::AuxRow &row, TpchQ16PartInput &out) {
  int64_t partkey = 0;
  if (!core::IntCell(row, 0, partkey) || !core::StringCell(row, 1, out.brand) ||
      !core::StringCell(row, 2, out.type))
    return false;
  int64_t size = 0;
  if (!core::IntCell(row, 3, size))
    return false;
  out.partkey = partkey;
  out.size = static_cast<int32_t>(size);
  return true;
}

bool ReadPartsupp(const core::AuxRow &row, TpchQ16PartsuppInput &out) {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  if (!core::IntCell(row, 0, partkey) || !core::IntCell(row, 1, suppkey))
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

const core::OutputSchema &ResultSchema() {
  static const core::OutputSchema schema{{{"p_brand", core::AuxType::String, false},
                                          {"p_type", core::AuxType::String, false},
                                          {"p_size", core::AuxType::Int64, false},
                                          {"supplier_cnt", core::AuxType::Int64, false}}};
  return schema;
}

// An output row of (brand, type, size, count) cells, filled in place per result.
core::AuxRow MakeResultRow() {
  core::AuxRow row;
  row.values.resize(4);
  for (std::size_t i = 0; i < 4; ++i) {
    row.values[i].type = i < 2 ? core::AuxType::String : core::AuxType::Int64;
    row.values[i].is_null = false;
  }
  row.values[0].payload = std::string();
  row.values[1].payload = std::string();
  row.values[2].payload = int64_t{0};
  row.values[3].payload = int64_t{0};
  return row;
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

  sink.Begin(ResultSchema());
  core::AuxRow row = MakeResultRow();
  const RowLess less{meta};
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
    ++best->cur;
    skip(*best);
  }
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

  sink.Begin(ResultSchema());
  core::AuxRow row = MakeResultRow();
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
  sink.End();
}

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

// A group's slot: (size, brand, type) flattened, sizes starting at 1.
struct GroupSlots {
  std::size_t brands, types;

  static constexpr std::size_t kSizes = static_cast<std::size_t>(Layout::kMaxSize);

  explicit GroupSlots(const TpchQ16Meta &meta) : brands(meta.brands.size()), types(meta.types.size()) {
    if (Count() > kMaxGroupSlots)
      throw std::runtime_error("TPC-H Q16: too many brand/type combinations");
  }
  std::size_t Count() const { return brands * types * kSizes; }
  std::size_t Of(int32_t size, uint16_t brand, uint16_t type) const {
    return (static_cast<std::size_t>(size - 1) * brands + brand) * types + type;
  }
  int32_t SizeOf(std::size_t slot) const { return static_cast<int32_t>(slot / (brands * types)) + 1; }
  uint16_t BrandOf(std::size_t slot) const { return static_cast<uint16_t>(slot / types % brands); }
  uint16_t TypeOf(std::size_t slot) const { return static_cast<uint16_t>(slot % types); }
};

SupplierPartial ScanSupplierPages(const core::AuxEngineServices &services,
                                  TpchQ16InitialScan &scan) {
  return core::PartitionedBuild<SupplierPartial>(
      services, [] { return SupplierPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanSuppliers(services, parts, [&](std::size_t p, const TpchQ16SupplierInput &input) {
          sink(p, core::SingleRowBatch<TpchQ16SupplierInput>{input});
        });
      },
      [](SupplierPartial &partial, const TpchQ16SupplierInput &input) {
        CheckKey(input.suppkey, "suppkey");
        partial.Page(input.suppkey)
            .eligible[static_cast<std::size_t>(input.suppkey) & (kSuppPageSize - 1)] =
            TpchQ16IsComplaintSupplier(input.comment) ? 0 : 1;
      },
      [](SupplierPartial &into, SupplierPartial &&from) { into.MergeFrom(std::move(from)); });
}

PartPartial ScanPartPages(const core::AuxEngineServices &services, TpchQ16InitialScan &scan) {
  return core::PartitionedBuild<PartPartial>(
      services, [] { return PartPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanParts(services, parts, [&](std::size_t p, const TpchQ16PartInput &input) {
          sink(p, core::SingleRowBatch<TpchQ16PartInput>{input});
        });
      },
      [](PartPartial &partial, const TpchQ16PartInput &input) {
        CheckKey(input.partkey, "partkey");
        TpchQ16PartInfo info;
        info.brand_id = InternName(partial.meta.brands, partial.meta.brand_index, input.brand,
                                   Layout::kBrandCap);
        info.type_id = InternName(partial.meta.types, partial.meta.type_index, input.type, kTypeCap);
        info.size = input.size;
        partial.Page(input.partkey).info[static_cast<std::size_t>(input.partkey) &
                                         (kPartPageSize - 1)] = info;
      },
      [](PartPartial &into, PartPartial &&from) { into.MergeFrom(std::move(from)); });
}

// Partsupp rows per page, and the members of each (size, brand, type) group.
PartsuppPartial ScanPartsupp(
    const core::AuxEngineServices &services, TpchQ16InitialScan &scan, const GroupSlots &slots,
    const std::vector<std::unique_ptr<TpchQ16PartPage>> &part_pages) {
  return core::PartitionedBuild<PartsuppPartial>(
      services, [&] { return PartsuppPartial(slots.Count()); },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanPartsupp(services, parts, [&](std::size_t p, const TpchQ16PartsuppInput &input) {
          sink(p, core::SingleRowBatch<TpchQ16PartsuppInput>{input});
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

        const auto part_chunk = static_cast<std::size_t>(input.partkey >> Layout::kPartShift);
        if (part_chunk >= part_pages.size() || !part_pages[part_chunk])
          return;
        const auto &info = part_pages[part_chunk]->info[static_cast<std::size_t>(input.partkey) &
                                                        (kPartPageSize - 1)];
        if (info.brand_id == kNoBrand || !SizeInRange(info.size))
          return;
        partial.AddMember(slots.Of(info.size, info.brand_id, info.type_id), info.type_id,
                          input.suppkey);
      },
      [](PartsuppPartial &into, PartsuppPartial &&from) { into.MergeFrom(std::move(from)); });
}

bool EligibleAt(const std::vector<std::unique_ptr<TpchQ16SupplierPage>> &supp_pages,
                int64_t suppkey) {
  const auto chunk = static_cast<std::size_t>(suppkey >> Layout::kSuppShift);
  if (chunk >= supp_pages.size() || !supp_pages[chunk])
    return true;
  return supp_pages[chunk]->eligible[static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1)] != 0;
}

// Counts each group's eligible suppliers.
void CountEligible(const core::AuxEngineServices &services,
                   const std::vector<std::unique_ptr<TpchQ16SupplierPage>> &supp_pages,
                   std::vector<TpchQ16GroupMembers> &groups) {
  core::ParallelEach(services, groups.size(), kGroupGrain, [&](std::size_t g) {
    auto &gm = groups[g];
    gm.eligible_count = static_cast<int32_t>(std::count_if(
        gm.suppkeys.begin(), gm.suppkeys.end(),
        [&](int64_t suppkey) { return EligibleAt(supp_pages, suppkey); }));
  });
}

// The groups each supplier belongs to, per supplier page.
std::vector<InvTemp> InvertGroups(const PartsuppPartial &partsupp, const GroupSlots &slots) {
  std::vector<InvTemp> inv_temp;
  for (std::size_t slot = 0; slot < partsupp.gindex.size(); ++slot) {
    if (partsupp.gindex[slot] == kNoGroup)
      continue;
    const auto code = GroupCode(slots.SizeOf(slot), slots.BrandOf(slot), slots.TypeOf(slot));
    for (const auto suppkey : partsupp.groups[partsupp.gindex[slot]].suppkeys) {
      const auto schunk = static_cast<std::size_t>(suppkey >> Layout::kSuppShift);
      if (schunk >= inv_temp.size())
        inv_temp.resize(schunk + 1);
      inv_temp[schunk].local.push_back(
          static_cast<uint16_t>(static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1)));
      inv_temp[schunk].code.push_back(code);
    }
  }
  return inv_temp;
}

// Group pages (one per size and brand) and the per-size sorted result lists.
// Releases the groups.
void AddGroupImages(PartsuppPartial &partsupp, const GroupSlots &slots, const TpchQ16Meta &meta,
                    std::vector<core::StateImage> &images) {
  std::vector<TpchQ16SizeList> lists(GroupSlots::kSizes + 1);
  for (int32_t size = Layout::kMinSize; size <= Layout::kMaxSize; ++size) {
    for (std::size_t brand = 0; brand < slots.brands; ++brand) {
      std::shared_ptr<TpchQ16GroupPage> page;
      for (std::size_t type = 0; type < slots.types; ++type) {
        const auto slot = partsupp.gindex[slots.Of(size, static_cast<uint16_t>(brand),
                                                   static_cast<uint16_t>(type))];
        if (slot == kNoGroup)
          continue;
        if (!page)
          page = std::make_shared<TpchQ16GroupPage>();
        auto &gm = partsupp.groups[slot];
        if (gm.eligible_count > 0)
          lists[static_cast<std::size_t>(size)].rows.push_back(
              {static_cast<uint16_t>(brand), static_cast<uint16_t>(type), size, gm.eligible_count});
        page->groups.push_back(std::move(gm));
      }
      if (page)
        images.push_back(MakeImage<TpchQ16GroupPage>(
            Layout::GroupPartition(size, static_cast<uint32_t>(brand)),
            std::shared_ptr<const TpchQ16GroupPage>(std::move(page))));
    }
  }
  partsupp.gindex = {};
  partsupp.groups = {};
  const RowLess less{meta};
  for (int32_t size = Layout::kMinSize; size <= Layout::kMaxSize; ++size) {
    auto &rows = lists[static_cast<std::size_t>(size)].rows;
    std::sort(rows.begin(), rows.end(), less);
    images.push_back(MakeImage<TpchQ16SizeList>(
        Layout::SizePartition(size),
        std::make_shared<const TpchQ16SizeList>(std::move(lists[static_cast<std::size_t>(size)]))));
  }
}

// Partsupp pages: CSR per chunk in parallel, images added in chunk order.
// Releases the temporaries.
void AddPartsuppImages(const core::AuxEngineServices &services, std::vector<PsTemp> &ps_temp,
                       std::vector<core::StateImage> &images) {
  std::vector<std::shared_ptr<const TpchQ16PartsuppPage>> ps_pages(ps_temp.size());
  core::ParallelEach(services, ps_temp.size(), kPsPageGrain, [&](std::size_t chunk) {
    auto &temp = ps_temp[chunk];
    if (temp.local.empty())
      return;
    auto page = std::make_shared<TpchQ16PartsuppPage>();
    BuildCsr(page->rows, kPsPageSize, temp.local, temp.sk);
    temp = PsTemp{};
    ps_pages[chunk] = std::move(page);
  });
  for (std::size_t chunk = 0; chunk < ps_pages.size(); ++chunk)
    if (ps_pages[chunk])
      images.push_back(MakeImage<TpchQ16PartsuppPage>(
          Layout::kPsBase + static_cast<core::PartitionId>(chunk), std::move(ps_pages[chunk])));
  ps_temp = {};
}

// Supplier pages: eligibility plus the groups each supplier belongs to.
// Releases the temporaries.
void AddSupplierImages(const core::AuxEngineServices &services,
                       std::vector<std::unique_ptr<TpchQ16SupplierPage>> &supp_pages,
                       std::vector<InvTemp> &inv_temp, std::vector<core::StateImage> &images) {
  const std::size_t chunks = std::max(supp_pages.size(), inv_temp.size());
  std::vector<std::shared_ptr<const TpchQ16SupplierPage>> supp_images(chunks);
  core::ParallelEach(services, chunks, kSuppPageGrain, [&](std::size_t chunk) {
    const bool has_page = chunk < supp_pages.size() && supp_pages[chunk];
    const bool has_inv = chunk < inv_temp.size() && !inv_temp[chunk].local.empty();
    if (!has_page && !has_inv)
      return;
    std::shared_ptr<TpchQ16SupplierPage> page;
    if (has_page) {
      page = std::shared_ptr<TpchQ16SupplierPage>(std::move(supp_pages[chunk]));
    } else {
      page = std::make_shared<TpchQ16SupplierPage>();
      page->Init();
    }
    if (has_inv) {
      BuildCsr(page->groups, kSuppPageSize, inv_temp[chunk].local, inv_temp[chunk].code);
      inv_temp[chunk] = InvTemp{};
    }
    supp_images[chunk] = std::move(page);
  });
  for (std::size_t chunk = 0; chunk < supp_images.size(); ++chunk)
    if (supp_images[chunk])
      images.push_back(MakeImage<TpchQ16SupplierPage>(
          Layout::kSuppBase + static_cast<core::PartitionId>(chunk), std::move(supp_images[chunk])));
  inv_temp = {};
}

// Part pages and the brand/type dictionaries.
void AddPartImages(std::vector<std::unique_ptr<TpchQ16PartPage>> &part_pages,
                   std::shared_ptr<TpchQ16Meta> meta, std::vector<core::StateImage> &images) {
  for (std::size_t chunk = 0; chunk < part_pages.size(); ++chunk) {
    if (!part_pages[chunk])
      continue;
    images.push_back(MakeImage<TpchQ16PartPage>(
        Layout::kPartBase + static_cast<core::PartitionId>(chunk),
        std::shared_ptr<const TpchQ16PartPage>(std::move(part_pages[chunk]))));
  }
  images.push_back(MakeImage<TpchQ16Meta>(Layout::kMeta,
                                          std::shared_ptr<const TpchQ16Meta>(std::move(meta))));
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
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &txn_buffer) {
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
  TpchQ16Stager stager(services);
  if (!stager.Open())
    return full_rebuild();
  if (core::SliceCount(services, buffer.Ops().size(), kOpGrain) > 1) {
    if (!stager.ApplyBatch(buffer.Ops()))
      return full_rebuild();
  } else {
    for (const auto &op : buffer.Ops())
      stager.Apply(op);
  }
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
  EmitMerged(*meta, lists, params, sink);
}

TpchQ16State TpchQ16Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q16 requires a typed initial scan source");

  auto suppliers = ScanSupplierPages(services, *initial_scan);
  auto parts = ScanPartPages(services, *initial_scan);
  auto meta = std::make_shared<TpchQ16Meta>(std::move(parts.meta));
  RecomputeRanks(*meta);

  const GroupSlots slots(*meta);
  auto partsupp = ScanPartsupp(services, *initial_scan, slots, parts.pages);
  CountEligible(services, suppliers.pages, partsupp.groups);
  auto inv_temp = InvertGroups(partsupp, slots);

  TpchQ16State state;
  AddGroupImages(partsupp, slots, *meta, state.images);
  AddPartsuppImages(services, partsupp.ps_temp, state.images);
  AddSupplierImages(services, suppliers.pages, inv_temp, state.images);
  AddPartImages(parts.pages, std::move(meta), state.images);
  return state;
}

std::vector<core::CalibrationWrite> TpchQ16Module::CalibrationWrites() const {
  return TpchQ16CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
