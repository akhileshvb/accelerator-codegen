#include "tpch/q13/postgres/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <limits>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {
namespace like = core::sql_like;

// LIKE semantics of the target DBMS. PostgreSQL LIKE is case-sensitive; the
// MySQL copy of this file sets this to true (default utf8mb4 collations are
// case-insensitive). This is the only intentional difference between the
// postgres/ and mysql/ copies. '_' matches one UTF-8 character in both.
constexpr bool kLikeCaseInsensitive = false;

// Segments per parallel slice (a segment is 4096 orders, ~100 us of matching).
constexpr std::size_t kSegmentGrain = 16;
// Customer-key chunks per slice of the histogram build; added orders per slice
// of the pattern-mask precompute (~1 us each).
constexpr std::size_t kChunkGrain = 4;
constexpr std::size_t kMaskGrain = 256;
constexpr std::size_t kSlotBits = 12; // log2(kTpchQ13SegmentRows)
static_assert(kTpchQ13SegmentRows == (std::size_t{1} << kSlotBits));

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

std::shared_ptr<const TpchQ13StateImage>
MakeStateImage(TpchQ13ImageKind kind,
               const std::shared_ptr<const TpchQ13State> &state) {
  auto image = std::make_shared<TpchQ13StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ13StateImage *AsQ13Image(const core::StateImage &image,
                                    TpchQ13ImageKind kind) {
  const auto *payload = image.As<TpchQ13StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

bool Q13OutputRowBetter(const TpchQ13OutputRow &lhs,
                        const TpchQ13OutputRow &rhs) {
  if (lhs.custdist != rhs.custdist)
    return lhs.custdist > rhs.custdist;
  return lhs.c_count > rhs.c_count;
}

constexpr std::string_view kWord1[4] = {"special", "pending", "unusual", "express"};
constexpr std::string_view kWord2[4] = {"packages", "requests", "accounts", "deposits"};

// First position >= from of `w` in buf[0, n), or -1. buf must be readable (and
// hold no letters) for 32 bytes past n. SSE2: a 16-position block is a
// candidate where both of w's first two bytes line up; candidates are checked
// with memcmp.
inline int64_t FindWord(const char *buf, std::size_t n, std::size_t from,
                        std::string_view w) {
#if defined(__SSE2__)
  const __m128i c0 = _mm_set1_epi8(w[0]);
  const __m128i c1 = _mm_set1_epi8(w[1]);
  for (std::size_t k = from; k + w.size() <= n; k += 16) {
    const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(buf + k));
    const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(buf + k + 1));
    uint32_t bits = static_cast<uint32_t>(_mm_movemask_epi8(
        _mm_and_si128(_mm_cmpeq_epi8(b0, c0), _mm_cmpeq_epi8(b1, c1))));
    while (bits) {
      const std::size_t pos = k + static_cast<std::size_t>(__builtin_ctz(bits));
      bits &= bits - 1;
      if (pos + w.size() > n)
        return -1;
      if (std::memcmp(buf + pos + 2, w.data() + 2, w.size() - 2) == 0)
        return static_cast<int64_t>(pos);
    }
  }
  return -1;
#else
  if (from > n)
    return -1;
  const void *hit = memmem(buf + from, n - from, w.data(), w.size());
  return hit ? static_cast<const char *>(hit) - buf : -1;
#endif
}

struct CustomerKeys {
  std::vector<int64_t> keys;
  int64_t max_key = -1;
};

// Orders packed into full segments, as AddOrder would fill them.
struct SegmentRun {
  std::vector<std::shared_ptr<TpchQ13OrderSegment>> segments;

  void Add(const TpchQ13OrderInput &input) {
    if (segments.empty() || segments.back()->orderkey.size() >= kTpchQ13SegmentRows) {
      auto segment = std::make_shared<TpchQ13OrderSegment>();
      segment->orderkey.reserve(kTpchQ13SegmentRows);
      segment->custkey.reserve(kTpchQ13SegmentRows);
      segment->alive.reserve(kTpchQ13SegmentRows);
      segment->comment.Reserve(kTpchQ13SegmentRows, 0);
      segments.push_back(std::move(segment));
    }
    auto &segment = *segments.back();
    segment.orderkey.push_back(input.orderkey);
    segment.custkey.push_back(input.custkey);
    segment.alive.push_back(1);
    segment.comment.Append(input.comment);
    ++segment.live;
  }
};

// excluded[custkey] = number of the customer's orders whose comment matches
// `matcher`: a flat array when the custkey range is dense enough, otherwise
// (and for negative / huge keys) a hash.
struct Excluded {
  std::vector<int32_t> flat;
  std::unordered_map<int64_t, int32_t> hash;
  void Add(int64_t key) {
    if (key >= 0 && static_cast<std::size_t>(key) < flat.size())
      ++flat[static_cast<std::size_t>(key)];
    else
      ++hash[key];
  }
  int32_t Get(int64_t key) const {
    if (key >= 0 && static_cast<std::size_t>(key) < flat.size())
      return flat[static_cast<std::size_t>(key)];
    const auto it = hash.find(key);
    return it == hash.end() ? 0 : it->second;
  }
};

// Custkeys of the live orders matching `matcher`, one SIMD pass per segment;
// slices collect their segments' keys, merged in slice order.
std::vector<int64_t> MatchingCustkeys(const TpchQ13State &state, const like::LikeMatcher &matcher,
                                      const core::AuxEngineServices &services) {
  std::vector<int64_t> matched;
  core::ParallelReduce(
      services, state.segments.size(), kSegmentGrain, matched,
      [&](std::vector<int64_t> &part, std::size_t begin, std::size_t end) {
        for (std::size_t s = begin; s < end; ++s) {
          const auto *seg = state.segments[s].get();
          if (!seg || seg->live == 0)
            continue;
          const std::size_t n = seg->orderkey.size();
          const auto bits =
              like::MatchBlobBitmap(matcher, seg->comment.offsets(), n, seg->comment.data());
          for (std::size_t w = 0; w < bits.size(); ++w) {
            uint64_t b = bits[w];
            while (b) {
              const std::size_t i = w * 64 + static_cast<std::size_t>(__builtin_ctzll(b));
              b &= b - 1;
              if (seg->alive[i])
                part.push_back(seg->custkey[i]);
            }
          }
        }
      },
      [](std::vector<int64_t> &into, std::vector<int64_t> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      });
  return matched;
}

} // namespace

int TpchQ13State::PatternIndex(std::string_view pattern) {
  // '%' w1 '%' w2 '%' exactly (no other wildcard / escape characters).
  if (pattern.size() < 3 || pattern.front() != '%' || pattern.back() != '%')
    return -1;
  for (int i = 0; i < 4; ++i) {
    const auto w1 = kWord1[i];
    if (pattern.size() < w1.size() + 3 || pattern.substr(1, w1.size()) != w1 ||
        pattern[1 + w1.size()] != '%')
      continue;
    const auto rest = pattern.substr(2 + w1.size(), pattern.size() - 3 - w1.size());
    for (int j = 0; j < 4; ++j)
      if (rest == kWord2[j])
        return i * 4 + j;
  }
  return -1;
}

// s LIKE '%a%b%' iff some occurrence of a ends at or before the start of some
// occurrence of b, i.e. (end of the first a) <= (start of the last b). Byte
// comparison is exact for PostgreSQL's case-sensitive LIKE on UTF-8 / single
// byte encodings (the words are ASCII).
uint32_t TpchQ13State::PatternMask(std::string_view comment) {
  static_assert(!kLikeCaseInsensitive);
  // Most comments contain no word1: 4 short SIMD scans. For each word1 found,
  // the end of its first occurrence is the best start for a word2.
  constexpr std::size_t kInline = 224;
  char local[kInline + 32];
  std::string padded;
  const char *d;
  const std::size_t n = comment.size();
  if (n <= kInline) {
    std::memcpy(local, comment.data(), n);
    std::memset(local + n, 0, 32);
    d = local;
  } else {
    padded.assign(comment.data(), n);
    padded.append(32, '\0');
    d = padded.data();
  }
  uint32_t mask = 0;
  for (int i = 0; i < 4; ++i) {
    const int64_t pos = FindWord(d, n, 0, kWord1[i]);
    if (pos < 0)
      continue;
    const auto end = static_cast<std::size_t>(pos) + kWord1[i].size();
    for (int j = 0; j < 4; ++j)
      if (FindWord(d, n, end, kWord2[j]) >= 0)
        mask |= uint32_t{1} << (i * 4 + j);
  }
  return mask;
}

void TpchQ13PatternHist::Merge(TpchQ13PatternHist &&from) {
  if (dense.size() < from.dense.size())
    dense.resize(from.dense.size(), 0);
  for (std::size_t i = 0; i < from.dense.size(); ++i)
    dense[i] += from.dense[i];
  for (const auto &[c_count, n] : from.big)
    Add(c_count, n);
}

void TpchQ13PatternHist::Add(int64_t c_count, int64_t delta) {
  if (c_count < kDenseLimit) {
    const auto i = static_cast<std::size_t>(c_count);
    if (dense.size() <= i)
      dense.resize(i + 1, 0);
    dense[i] += delta;
    return;
  }
  auto &v = big[c_count];
  v += delta;
  if (v == 0)
    big.erase(c_count);
}

TpchQ13State::PatternCounts TpchQ13State::Matched(int64_t key) const {
  if (Dense(key))
    return matched.Get(key);
  const PatternCounts *m = sparse_matched.Find(key);
  return m ? *m : PatternCounts{};
}

// Adds (sign = +1) / removes (-1) customer `custkey` to / from every pattern
// histogram at its current effective count total - matched[p].
void TpchQ13State::HistAdd(int64_t custkey, int64_t sign) {
  HistAddTo(hist, custkey, sign);
}

void TpchQ13State::HistAddTo(PatternHists &into, int64_t custkey, int64_t sign) const {
  const int64_t total = Count(custkey);
  const PatternCounts m = Matched(custkey);
  for (std::size_t p = 0; p < kTpchQ13Patterns; ++p)
    into[p].Add(total - m[p], sign);
}

void TpchQ13State::DisablePatternIndex() {
  patterns_ready = false;
  matched.Clear();
  sparse_matched = {};
  hist = {};
}

// An order delta of `sign` for `custkey` whose comment matches the patterns in
// `mask`: c_count changes by `sign` exactly for the patterns NOT in `mask`.
void TpchQ13State::ApplyOrderCount(int64_t custkey, int32_t sign, uint32_t mask) {
  if (!patterns_ready) {
    AddCount(custkey, sign);
    return;
  }
  const int64_t total = Count(custkey);
  AddCount(custkey, sign);
  PatternCounts m = Matched(custkey);
  if (mask != 0) {
    PatternCounts next = m;
    for (uint32_t b = mask; b; b &= b - 1) {
      const int p = __builtin_ctz(b);
      const int v = static_cast<int>(next[p]) + sign;
      if (v < 0 || v > 255) {
        DisablePatternIndex();
        return;
      }
      next[p] = static_cast<uint8_t>(v);
    }
    (Dense(custkey) ? matched.Mutable(custkey) : sparse_matched[custkey]) = next;
  }
  if (!Valid(custkey))
    return;
  for (std::size_t p = 0; p < kTpchQ13Patterns; ++p) {
    if (mask >> p & 1)
      continue;
    const int64_t eff = total - m[p];
    hist[p].Add(eff, -1);
    hist[p].Add(eff + sign, +1);
  }
}

void TpchQ13State::EnablePatternIndex(const core::AuxEngineServices &services) {
  DisablePatternIndex();
  // 1. Match masks of every order, segments in parallel.
  const std::size_t nseg = segments.size();
  std::vector<std::vector<uint16_t>> masks(nseg);
  core::ParallelSlices(services, nseg, kSegmentGrain,
                       [&](std::size_t, std::size_t begin, std::size_t end) {
    for (std::size_t s = begin; s < end; ++s) {
      const auto *seg = segments[s].get();
      if (!seg || seg->live == 0)
        continue;
      auto &out = masks[s];
      out.assign(seg->orderkey.size(), 0);
      for (std::size_t i = 0; i < out.size(); ++i)
        if (seg->alive[i])
          out[i] = static_cast<uint16_t>(PatternMask(seg->comment.View(i)));
    }
  });
  // 2. Per-customer match counts.
  for (std::size_t s = 0; s < nseg; ++s) {
    const auto &mk = masks[s];
    for (std::size_t i = 0; i < mk.size(); ++i) {
      if (!mk[i])
        continue;
      const int64_t custkey = segments[s]->custkey[i];
      PatternCounts &m = Dense(custkey) ? matched.Mutable(custkey)
                                        : sparse_matched[custkey];
      for (uint32_t b = mk[i]; b; b &= b - 1) {
        const int p = __builtin_ctz(b);
        if (m[p] == 255) {
          DisablePatternIndex();
          return;
        }
        ++m[p];
      }
    }
    std::vector<uint16_t>().swap(masks[s]);
  }
  // 3. Histograms over the valid customers: chunk ranges in parallel, each
  // slice into its own histograms.
  using ValidArray = core::ChunkedCowArray<uint8_t>;
  core::ParallelReduce(
      services, customer_valid.NumChunks(), kChunkGrain, hist,
      [&](PatternHists &into, std::size_t begin, std::size_t end) {
        for (std::size_t ci = begin; ci < end; ++ci) {
          const auto *valid = customer_valid.ChunkAt(ci);
          if (!valid)
            continue;
          const int64_t base = static_cast<int64_t>(ci * ValidArray::kChunkSize);
          for (std::size_t j = 0; j < valid->size(); ++j)
            if ((*valid)[j])
              HistAddTo(into, base + static_cast<int64_t>(j), +1);
        }
      },
      [](PatternHists &into, PatternHists &&from) {
        for (std::size_t p = 0; p < kTpchQ13Patterns; ++p)
          into[p].Merge(std::move(from[p]));
      });
  sparse_customers.ForEach([&](int64_t key, const SparseSlot &slot) {
    if (slot.valid)
      HistAdd(key, +1);
  });
  patterns_ready = true;
}

int32_t TpchQ13State::Count(int64_t key) const {
  if (Dense(key))
    return order_count.Get(key);
  const SparseSlot *s = sparse_customers.Find(key);
  return s ? s->count : 0;
}
bool TpchQ13State::Valid(int64_t key) const {
  if (Dense(key))
    return customer_valid.Get(key) != 0;
  const SparseSlot *s = sparse_customers.Find(key);
  return s && s->valid;
}
void TpchQ13State::AddCount(int64_t key, int32_t delta) {
  if (Dense(key))
    order_count.Mutable(key) += delta;
  else
    sparse_customers[key].count += delta;
}
void TpchQ13State::SetValid(int64_t key, bool valid) {
  if (Dense(key))
    customer_valid.Set(key, valid ? 1 : 0);
  else
    sparse_customers[key].valid = valid ? 1 : 0;
}

// Dense columns when the keys fill at least a quarter of their range (plus a
// small floor), else everything is sparse.
void TpchQ13State::SizeDenseKeys(int64_t customer_count, int64_t max_custkey) {
  dense_limit = max_custkey >= 0 && max_custkey / 4 <= customer_count + (int64_t{1} << 18)
                    ? max_custkey + 1 + (max_custkey >> 3)
                    : 0;
}

void TpchQ13State::AddCustomer(int64_t custkey) {
  if (Valid(custkey))
    return;
  SetValid(custkey, true);
  ++customers;
  if (patterns_ready)
    HistAdd(custkey, +1);
}

void TpchQ13State::RemoveCustomer(int64_t custkey) {
  if (!Valid(custkey))
    return;
  if (patterns_ready)
    HistAdd(custkey, -1);
  SetValid(custkey, false);
  --customers;
}

std::shared_ptr<TpchQ13OrderSegment> &TpchQ13State::MutableSegment(uint32_t id) {
  auto &segment = segments[id];
  if (segment.use_count() > 1)
    segment = std::make_shared<TpchQ13OrderSegment>(*segment);
  return segment;
}

// Orders count toward their custkey whether or not that customer exists yet
// (the join only sees a customer row), so customer and order maintenance are
// independent and a customer insert/delete is a single flag flip.
void TpchQ13State::AddOrder(int64_t orderkey, int64_t custkey,
                            std::string_view comment, uint32_t pattern_mask) {
  RemoveOrder(orderkey); // primary key: re-adding an existing key replaces it
  if (open_segment < 0 ||
      segments[static_cast<std::size_t>(open_segment)]->orderkey.size() >=
          kTpchQ13SegmentRows) {
    if (!free_segments.empty()) {
      open_segment = free_segments.back();
      free_segments.pop_back();
    } else {
      open_segment = static_cast<int64_t>(segments.size());
      segments.emplace_back();
    }
    auto &segment = segments[static_cast<std::size_t>(open_segment)];
    segment = std::make_shared<TpchQ13OrderSegment>();
    segment->orderkey.reserve(kTpchQ13SegmentRows);
    segment->custkey.reserve(kTpchQ13SegmentRows);
    segment->alive.reserve(kTpchQ13SegmentRows);
    segment->comment.Reserve(kTpchQ13SegmentRows, 0);
  }
  const auto id = static_cast<uint32_t>(open_segment);
  auto &segment = *MutableSegment(id);
  const std::size_t slot = segment.orderkey.size();
  segment.orderkey.push_back(orderkey);
  segment.custkey.push_back(custkey);
  segment.alive.push_back(1);
  segment.comment.Append(comment);
  ++segment.live;
  order_row.Upsert(orderkey, (static_cast<uint64_t>(id) << kSlotBits) | slot);
  ApplyOrderCount(custkey, 1, pattern_mask);
  ++live_orders;
}

void TpchQ13State::AdoptSegments(
    std::vector<std::shared_ptr<TpchQ13OrderSegment>> &&adopted) {
  for (auto &adopt : adopted) {
    const auto id = static_cast<uint32_t>(segments.size());
    segments.push_back(std::move(adopt));
    const auto &segment = *segments.back();
    for (std::size_t slot = 0; slot < segment.orderkey.size(); ++slot) {
      RemoveOrder(segment.orderkey[slot]); // primary key: a repeated key replaces it
      order_row.Upsert(segment.orderkey[slot],
                       (static_cast<uint64_t>(id) << kSlotBits) | slot);
      AddCount(segment.custkey[slot], 1);
      ++live_orders;
    }
  }
}

void TpchQ13State::RemoveOrder(int64_t orderkey) {
  const uint64_t *found = order_row.Find(orderkey);
  if (!found)
    return;
  const auto id = static_cast<uint32_t>(*found >> kSlotBits);
  const std::size_t slot = *found & (kTpchQ13SegmentRows - 1);
  auto &segment = *MutableSegment(id);
  segment.alive[slot] = 0;
  --segment.live;
  ApplyOrderCount(segment.custkey[slot], -1,
                  patterns_ready ? PatternMask(segment.comment.View(slot)) : 0);
  order_row.Erase(orderkey);
  --live_orders;
  touched_.push_back(id);
}

// A segment that is mostly dead is rewritten without its dead rows (moved rows
// get new index entries); an empty one is released for reuse. Cost O(segment).
void TpchQ13State::CompactSegment(uint32_t id) {
  auto &slot_ptr = segments[id];
  if (!slot_ptr)
    return;
  const auto &old = *slot_ptr;
  const std::size_t rows = old.orderkey.size();
  if (old.live == 0) {
    slot_ptr.reset();
    free_segments.push_back(id);
    if (open_segment == static_cast<int64_t>(id))
      open_segment = -1;
    return;
  }
  if ((rows - old.live) * 2 <= rows)
    return;
  auto fresh = std::make_shared<TpchQ13OrderSegment>();
  fresh->orderkey.reserve(old.live);
  fresh->custkey.reserve(old.live);
  fresh->alive.reserve(old.live);
  fresh->comment.Reserve(old.live, old.comment.bytes());
  for (std::size_t i = 0; i < rows; ++i) {
    if (!old.alive[i])
      continue;
    const std::size_t slot = fresh->orderkey.size();
    fresh->orderkey.push_back(old.orderkey[i]);
    fresh->custkey.push_back(old.custkey[i]);
    fresh->alive.push_back(1);
    fresh->comment.Append(old.comment.View(i));
    if (slot != i)
      order_row.Upsert(old.orderkey[i],
                       (static_cast<uint64_t>(id) << kSlotBits) | slot);
  }
  fresh->live = old.live;
  slot_ptr = std::move(fresh);
}

void TpchQ13State::CompactTouched() {
  std::sort(touched_.begin(), touched_.end());
  touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
  for (const uint32_t id : touched_)
    CompactSegment(id);
  touched_.clear();
}

bool TpchQ13TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ13TxnBuffer::Clear() {
  pending = false;
  deltas.clear();
}
const std::vector<TpchQ13Delta> &TpchQ13TxnBuffer::Deltas() const { return deltas; }
void TpchQ13TxnBuffer::AddDelta(TpchQ13Delta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}

core::AcceleratorId TpchQ13Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ13Module::Dependencies() const {
  // orders projection: o_orderkey, o_custkey, o_comment.
  return {
      {config.customer_table, {0}, {0}},
      {config.orders_table, {0, 1, 2}, {0, 1, 8}},
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ13Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ13TxnBuffer>();
}

void TpchQ13Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, 0,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts, state)));
  builder.Finish();
}

std::shared_ptr<TpchQ13State>
TpchQ13Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ13State>();
  const std::size_t partitions = core::BuildPartitions(services);
  const auto customers = core::PartitionedBuild<CustomerKeys>(
      services, [] { return CustomerKeys{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomers(
            services, parts, [&](std::size_t partition, const TpchQ13CustomerInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ13CustomerInput>{input});
            });
      },
      [](CustomerKeys &into, const TpchQ13CustomerInput &input) {
        into.keys.push_back(input.key);
        into.max_key = std::max(into.max_key, input.key);
      },
      [](CustomerKeys &into, CustomerKeys &&from) {
        into.keys.insert(into.keys.end(), from.keys.begin(), from.keys.end());
        into.max_key = std::max(into.max_key, from.max_key);
      },
      partitions);
  state->SizeDenseKeys(static_cast<int64_t>(customers.keys.size()), customers.max_key);
  for (const int64_t key : customers.keys)
    state->AddCustomer(key);
  // Each partition fills its own segments; the state indexes them in order.
  auto runs = core::PartitionedBuild<SegmentRun>(
      services, [] { return SegmentRun{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(
            services, parts, [&](std::size_t partition, const TpchQ13OrderInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ13OrderInput>{input});
            });
      },
      [](SegmentRun &into, const TpchQ13OrderInput &input) { into.Add(input); },
      [](SegmentRun &into, SegmentRun &&from) {
        into.segments.insert(into.segments.end(), from.segments.begin(),
                             from.segments.end());
      },
      partitions);
  state->AdoptSegments(std::move(runs.segments));
  state->EnablePatternIndex(services);
  return state;
}

core::InvalidationPlan
TpchQ13Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table)
    return All();
  if (mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ13Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);

  auto add_customer = [&](const core::AuxRow &row, int8_t sign) {
    int64_t custkey = 0;
    if (!core::IntCell(row, 0, custkey))
      return; // a NULL key never joins
    TpchQ13Delta delta;
    delta.table = TpchQ13Delta::Table::Customer;
    delta.sign = sign;
    delta.orderkey = custkey;
    buffer.AddDelta(std::move(delta));
  };
  // An order with a NULL orderkey/custkey/comment never contributes to any
  // count (the join predicate is not true), so it is not tracked at all.
  auto add_order = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ13Delta delta;
    delta.sign = sign;
    if (!core::IntCell(row, 0, delta.orderkey) || !core::IntCell(row, 1, delta.custkey) ||
        !core::StringCell(row, 2, delta.comment))
      return;
    buffer.AddDelta(std::move(delta));
  };

  const bool is_customer = mutation.table == config.customer_table;
  if (!is_customer && mutation.table != config.orders_table)
    return;
  auto apply = [&](const core::AuxRow &row, int8_t sign) {
    if (is_customer)
      add_customer(row, sign);
    else
      add_order(row, sign);
  };

  // An update is "all old rows removed, then all new rows added": identical to
  // pairwise handling, and immune to key swaps within one statement.
  if (mutation.kind != core::MutationKind::Insert)
    for (const auto &row : mutation.old_rows)
      apply(row, -1);
  if (mutation.kind != core::MutationKind::Delete)
    for (const auto &row : mutation.new_rows)
      apply(row, 1);
}

core::StagedMaintenanceOutcome
TpchQ13Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
      buffer.Deltas().size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ13Layout::kCustomerCounts});

  std::shared_ptr<TpchQ13State> updated_state;
  core::Version existing_version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return outcome;
    // Copy-on-write: shares every segment / chunk / index shard with the
    // published state; only the ones touched below are cloned.
    updated_state = std::make_shared<TpchQ13State>(*typed->state);
    existing_version = image.version;
    break;
  }
  if (!updated_state)
    return outcome;

  // An added order's pattern mask depends on its comment alone: compute them
  // in parallel slices, then apply the deltas in order.
  const auto &deltas = buffer.Deltas();
  std::vector<uint32_t> masks(deltas.size(), 0);
  if (updated_state->patterns_ready)
    core::ParallelEach(services, deltas.size(), kMaskGrain, [&](std::size_t i) {
      if (deltas[i].table == TpchQ13Delta::Table::Order && deltas[i].sign > 0)
        masks[i] = TpchQ13State::PatternMask(deltas[i].comment);
    });
  for (std::size_t i = 0; i < deltas.size(); ++i) {
    const auto &delta = deltas[i];
    if (delta.table == TpchQ13Delta::Table::Customer) {
      if (delta.sign > 0)
        updated_state->AddCustomer(delta.orderkey);
      else
        updated_state->RemoveCustomer(delta.orderkey);
    } else if (delta.sign > 0) {
      updated_state->AddOrder(delta.orderkey, delta.custkey, delta.comment, masks[i]);
    } else {
      updated_state->RemoveOrder(delta.orderkey);
    }
  }
  updated_state->CompactTouched();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, existing_version,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts,
                     std::static_pointer_cast<const TpchQ13State>(updated_state))));
  return outcome;
}

bool TpchQ13Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ13Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  std::shared_ptr<const TpchQ13State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(*state, arguments, services, sink);
}

bool TpchQ13Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ13State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ13Layout::kCustomerCounts});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return false;
    state = typed->state;
    return true;
  }
  return false;
}

void TpchQ13Module::EmitResult(const TpchQ13State &state,
                               const core::QueryArguments &arguments,
                               const core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  const bool pattern_null =
      arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload);

  auto emit = [&](std::vector<TpchQ13OutputRow> &rows) {
    std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
    sink.Begin({{{"c_count", core::AuxType::Int64, false},
                 {"custdist", core::AuxType::Int64, false}}});
    for (const auto &r : rows)
      sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                      core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
    sink.End();
  };

  // Fast paths, O(result rows): a NULL pattern (every customer has c_count 0)
  // and the known patterns (maintained histograms).
  if (pattern_null) {
    std::vector<TpchQ13OutputRow> rows;
    if (state.customers > 0)
      rows.push_back({0, state.customers});
    emit(rows);
    return;
  }
  if (state.patterns_ready) {
    const int p = TpchQ13State::PatternIndex(
        std::get<std::string>(arguments.values[0].payload));
    if (p >= 0) {
      const auto &h = state.hist[static_cast<std::size_t>(p)];
      std::vector<TpchQ13OutputRow> rows;
      for (std::size_t c = 0; c < h.dense.size(); ++c)
        if (h.dense[c] > 0)
          rows.push_back({static_cast<int64_t>(c), h.dense[c]});
      for (const auto &[c, n] : h.big)
        if (n > 0)
          rows.push_back({c, n});
      emit(rows);
      return;
    }
  }

  using Array = core::ChunkedCowArray<int32_t>;
  const std::size_t span =
      std::max(state.order_count.NumChunks(), state.customer_valid.NumChunks()) *
      Array::kChunkSize;

  // Count the matching orders per customer.
  Excluded excluded;
  const auto matcher = like::LikeMatcher::Compile(
      std::get<std::string>(arguments.values[0].payload), '\\', kLikeCaseInsensitive,
      /*utf8_underscore=*/true);
  if (span <= static_cast<std::size_t>(state.customers) * 4 + (std::size_t{1} << 20))
    excluded.flat.assign(span, 0);
  for (const int64_t custkey : MatchingCustkeys(state, matcher, services))
    excluded.Add(custkey);

  // Histogram of effective counts. Counts up to kDense use a flat array; the
  // (never expected) larger ones fall into a sparse map.
  constexpr int64_t kDense = int64_t{1} << 22;
  int64_t max_count = 0;
  for (std::size_t ci = 0; ci < state.order_count.NumChunks(); ++ci)
    if (const auto *chunk = state.order_count.ChunkAt(ci))
      for (const int32_t c : *chunk)
        max_count = std::max<int64_t>(max_count, c);
  state.sparse_customers.ForEach([&](int64_t, const TpchQ13State::SparseSlot &slot) {
    max_count = std::max<int64_t>(max_count, slot.count);
  });
  std::vector<int64_t> dense(static_cast<std::size_t>(std::min(max_count, kDense)) + 1, 0);
  std::map<int64_t, int64_t> sparse;

  for (std::size_t ci = 0; ci < state.customer_valid.NumChunks(); ++ci) {
    const auto *valid = state.customer_valid.ChunkAt(ci);
    if (!valid)
      continue;
    const auto *counts = state.order_count.ChunkAt(ci);
    const std::size_t base = ci * Array::kChunkSize;
    for (std::size_t j = 0; j < valid->size(); ++j) {
      if (!(*valid)[j])
        continue;
      const int64_t eff = (counts ? (*counts)[j] : 0) - excluded.Get(static_cast<int64_t>(base + j));
      if (eff <= kDense)
        ++dense[static_cast<std::size_t>(eff)];
      else
        ++sparse[eff];
    }
  }
  state.sparse_customers.ForEach([&](int64_t key, const TpchQ13State::SparseSlot &slot) {
    if (!slot.valid)
      return;
    const int64_t eff = slot.count - excluded.Get(key);
    if (eff <= kDense)
      ++dense[static_cast<std::size_t>(eff)];
    else
      ++sparse[eff];
  });

  std::vector<TpchQ13OutputRow> rows;
  for (std::size_t c = 0; c < dense.size(); ++c)
    if (dense[c] > 0)
      rows.push_back({static_cast<int64_t>(c), dense[c]});
  for (const auto &[c, n] : sparse)
    rows.push_back({c, n});
  emit(rows);
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
