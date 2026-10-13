#include "sec_edgar/q2/duckdb/aux/state.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::query {
namespace {

size_t NextPow2(size_t v) {
  size_t p = 16;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

size_t IntSqrt(size_t v) {
  size_t r = static_cast<size_t>(std::sqrt(static_cast<double>(v)));
  while (r * r > v) {
    --r;
  }
  while ((r + 1) * (r + 1) <= v) {
    ++r;
  }
  return r;
}

// Automatic overlay limit of a slice with `n` base groups.  A compaction
// costs O(n) and a read scans the whole overlay of its unit of measure, while
// a write copies only the overlay shards it touches.  Small slices let the
// overlay reach n/4 (cheap to scan, rare compactions), large ones n/32 (a
// read scans a few percent of the slice; each compaction is amortised over
// n/32 written groups).
uint64_t AutoOverlayLimit(uint64_t n) {
  return std::max<uint64_t>({1024, 4 * IntSqrt(n), std::min<uint64_t>(n / 4, 65536), n / 32});
}

// Shards so that a full overlay has at most ~256 entries per shard (the
// first write of a version to a shard copies it).
unsigned ShardShiftFor(uint64_t n) {
  const uint64_t want = AutoOverlayLimit(n) / 256;
  unsigned bits = 0;
  while (bits < 16 && (uint64_t{1} << bits) < want) {
    ++bits;
  }
  return bits == 0 ? 64 : 64 - bits;
}

} // namespace

// ---------------------------------------------------------------- Q2Dict

Q2Dict::Q2Dict() : base_(std::make_shared<const core::StringDict>()) {}

uint32_t Q2Dict::Find(std::string_view s) const {
  const uint32_t id = base_->Find(s);
  if (id != core::StringDict::kNone) {
    return id;
  }
  if (delta_) {
    const uint32_t d = delta_->Find(s);
    if (d != core::StringDict::kNone) {
      return base_n_ + d;
    }
  }
  return kQ2None;
}

uint32_t Q2Dict::GetOrAdd(std::string_view s) {
  const uint32_t found = Find(s);
  if (found != kQ2None) {
    return found;
  }
  if (!delta_) {
    delta_ = std::make_shared<core::StringDict>();
  } else if (delta_.use_count() > 1) {
    delta_ = std::make_shared<core::StringDict>(*delta_); // copy-on-write
  }
  return base_n_ + delta_->Intern(s);
}

void Q2Dict::Compact() {
  if (!delta_) {
    return;
  }
  if (base_n_ == 0 && delta_.use_count() == 1) {
    base_ = std::make_shared<const core::StringDict>(std::move(*delta_));
  } else {
    auto merged = std::make_shared<core::StringDict>(*base_);
    for (uint32_t i = 0; i < delta_->size(); ++i) {
      merged->Intern(delta_->Get(i));
    }
    base_ = std::move(merged);
  }
  delta_.reset();
  base_n_ = static_cast<uint32_t>(base_->size());
}

size_t Q2Dict::HeapBytes() const {
  return base_->MemoryBytes() + (delta_ ? delta_->MemoryBytes() : 0);
}

// ------------------------------------------------------------- group ops

bool Q2GroupInsert(Q2Group &g, int64_t key, uint64_t n) {
  constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
  if (n > kMax) {
    return false;
  }
  const uint32_t add = static_cast<uint32_t>(n);
  if (g.c0 == 0) {
    g.k0 = key;
    g.c0 = add;
    g.k1 = 0;
    g.c1 = 0;
    g.trunc = false;
    return true;
  }
  if (key > g.k0) {
    if (g.c1 > 0) {
      g.trunc = true; // previous second value becomes unlisted
    }
    g.k1 = g.k0;
    g.c1 = g.c0;
    g.k0 = key;
    g.c0 = add;
    return true;
  }
  if (key == g.k0) {
    if (g.c0 + n > kMax) {
      return false;
    }
    g.c0 += add;
    return true;
  }
  if (g.c1 > 0) {
    if (key == g.k1) {
      if (g.c1 + n > kMax) {
        return false;
      }
      g.c1 += add;
    } else if (key > g.k1) {
      g.k1 = key; // old second value becomes unlisted
      g.c1 = add;
      g.trunc = true;
    } else {
      g.trunc = true;
    }
    return true;
  }
  if (!g.trunc) {
    g.k1 = key;
    g.c1 = add;
  }
  // else: unlisted row below the only listed value; nothing to record.
  return true;
}

Q2Removal Q2GroupRemove(Q2Group &g, int64_t key) {
  if (g.c0 == 0 || key > g.k0) {
    return Q2Removal::Inconsistent;
  }
  if (key == g.k0) {
    if (--g.c0 > 0) {
      return Q2Removal::Ok;
    }
    if (g.c1 > 0) {
      g.k0 = g.k1;
      g.c0 = g.c1;
      g.k1 = 0;
      g.c1 = 0;
      return Q2Removal::Ok;
    }
    return g.trunc ? Q2Removal::Lost : Q2Removal::Ok;
  }
  if (g.c1 > 0) {
    if (key == g.k1) {
      if (--g.c1 == 0) {
        g.k1 = 0;
      }
      return Q2Removal::Ok;
    }
    if (key > g.k1) {
      return Q2Removal::Inconsistent;
    }
  }
  return g.trunc ? Q2Removal::Ok : Q2Removal::Inconsistent;
}

// --------------------------------------------------------- Q2GroupTable

int32_t Q2GroupTable::Find(uint32_t adsh_id, uint32_t tag_id) const {
  if (slots.empty()) {
    return -1;
  }
  const size_t mask = slots.size() - 1;
  size_t pos = Q2PairHash(adsh_id, tag_id) & mask;
  while (true) {
    const uint32_t v = slots[pos];
    if (v == 0) {
      return -1;
    }
    const Q2Group &g = ents[v - 1];
    if (g.adsh == adsh_id && g.tag == tag_id) {
      return static_cast<int32_t>(v - 1);
    }
    pos = (pos + 1) & mask;
  }
}

void Q2GroupTable::Put(const Q2Group &g) {
  const int32_t e = Find(g.adsh, g.tag);
  if (e >= 0) {
    ents[e] = g;
    return;
  }
  Append(g);
}

void Q2GroupTable::Append(const Q2Group &g) {
  if ((ents.size() + 1) * 2 > slots.size()) {
    const size_t cap = NextPow2((ents.size() + 1) * 2);
    slots.assign(cap, 0);
    const size_t mask = cap - 1;
    for (uint32_t i = 0; i < ents.size(); ++i) {
      size_t pos = Q2PairHash(ents[i].adsh, ents[i].tag) & mask;
      while (slots[pos] != 0) {
        pos = (pos + 1) & mask;
      }
      slots[pos] = i + 1;
    }
  }
  ents.push_back(g);
  const size_t mask = slots.size() - 1;
  size_t pos = Q2PairHash(g.adsh, g.tag) & mask;
  while (slots[pos] != 0) {
    pos = (pos + 1) & mask;
  }
  slots[pos] = static_cast<uint32_t>(ents.size());
}

// --------------------------------------------------------------- Q2Base

unsigned Q2Base::ShiftFor(size_t count) {
  // Load factor <= ~0.7.
  const size_t cap = NextPow2(count + count / 2 + 1);
  return 64u - static_cast<unsigned>(__builtin_ctzll(cap));
}

void Q2Base::Reserve(size_t count) {
  k0.reserve(count);
  k1.reserve(count);
  adsh.reserve(count);
  tag.reserve(count);
  c0b.reserve(count);
  c1b.reserve(count);
}

void Q2Base::PushBack(const Q2Group &g) {
  if (n == std::numeric_limits<uint32_t>::max() - 1) {
    throw std::length_error("sec_edgar q2: more than 2^32-2 groups in one unit of measure");
  }
  const uint32_t i = n++;
  k0.push_back(g.k0);
  k1.push_back(g.k1);
  adsh.push_back(g.adsh);
  tag.push_back(g.tag);
  const bool esc0 = g.c0 >= kC0Esc;
  const bool esc1 = g.c1 >= kC1Esc;
  c0b.push_back(esc0 ? kC0Esc : static_cast<uint8_t>(g.c0));
  c1b.push_back(static_cast<uint8_t>((esc1 ? kC1Esc : g.c1) | (g.trunc ? kTruncBit : 0)));
  if (esc0 || esc1) {
    e_pos.push_back(i);
    e_c0.push_back(g.c0);
    e_c1.push_back(g.c1);
  }
}

void Q2Base::Finish() {
  shift = ShiftFor(n);
  const size_t cap = size_t{1} << (64 - shift);
  slots.assign(cap, 0);
  const size_t mask = cap - 1;
  constexpr uint32_t kAhead = 16;
  for (uint32_t i = 0; i < n; ++i) {
    if (i + kAhead < n) {
      __builtin_prefetch(slots.data() + (Q2PairHash(adsh[i + kAhead], tag[i + kAhead]) >> shift), 1, 0);
    }
    size_t pos = static_cast<size_t>(Q2PairHash(adsh[i], tag[i]) >> shift);
    while (slots[pos] != 0) {
      pos = (pos + 1) & mask;
    }
    slots[pos] = i + 1;
  }
}

std::shared_ptr<const Q2Base> Q2Base::Build(std::vector<Q2Group> &&groups) {
  if (groups.size() >= std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("sec_edgar q2: more than 2^32-2 groups in one unit of measure");
  }
  std::sort(groups.begin(), groups.end(), [](const Q2Group &a, const Q2Group &b) {
    if (a.k0 != b.k0) {
      return a.k0 > b.k0;
    }
    if (a.adsh != b.adsh) {
      return a.adsh < b.adsh;
    }
    return a.tag < b.tag;
  });
  auto base = std::make_shared<Q2Base>();
  base->Reserve(groups.size());
  for (const Q2Group &g : groups) {
    base->PushBack(g);
  }
  std::vector<Q2Group>().swap(groups);
  base->Finish();
  return base;
}

int32_t Q2Base::Find(uint32_t adsh_id, uint32_t tag_id) const {
  if (n == 0) {
    return -1;
  }
  const size_t mask = slots.size() - 1;
  size_t pos = static_cast<size_t>(Q2PairHash(adsh_id, tag_id) >> shift);
  while (true) {
    const uint32_t v = slots[pos];
    if (v == 0) {
      return -1;
    }
    const uint32_t i = v - 1;
    if (adsh[i] == adsh_id && tag[i] == tag_id) {
      return static_cast<int32_t>(i);
    }
    pos = (pos + 1) & mask;
  }
}

uint32_t Q2Base::EscapedC0(uint32_t i) const {
  const auto it = std::lower_bound(e_pos.begin(), e_pos.end(), i);
  return e_c0[static_cast<size_t>(it - e_pos.begin())];
}

void Q2Base::Fill(Q2Group &g, uint32_t i, size_t e) const {
  g.k0 = k0[i];
  g.k1 = k1[i];
  g.adsh = adsh[i];
  g.tag = tag[i];
  const bool esc = (c0b[i] == kC0Esc || (c1b[i] & ~kTruncBit) == kC1Esc);
  g.c0 = c0b[i];
  g.c1 = c1b[i] & ~kTruncBit;
  g.trunc = (c1b[i] & kTruncBit) != 0;
  if (esc) {
    g.c0 = e_c0[e];
    g.c1 = e_c1[e];
  }
}

Q2Group Q2Base::Get(uint32_t i, uint32_t uom) const {
  Q2Group g;
  g.uom = uom;
  size_t e = 0;
  if (c0b[i] == kC0Esc || (c1b[i] & ~kTruncBit) == kC1Esc) {
    e = static_cast<size_t>(std::lower_bound(e_pos.begin(), e_pos.end(), i) - e_pos.begin());
  }
  Fill(g, i, e);
  return g;
}

Q2Group Q2Base::GetSeq(uint32_t i, uint32_t uom, size_t &ec) const {
  Q2Group g;
  g.uom = uom;
  if (c0b[i] == kC0Esc || (c1b[i] & ~kTruncBit) == kC1Esc) {
    while (ec < e_pos.size() && e_pos[ec] < i) {
      ++ec;
    }
  }
  Fill(g, i, ec);
  return g;
}

size_t Q2Base::HeapBytes() const {
  return k0.capacity() * 8 + k1.capacity() * 8 + adsh.capacity() * 4 +
         tag.capacity() * 4 + c0b.capacity() + c1b.capacity() +
         (e_pos.capacity() + e_c0.capacity() + e_c1.capacity()) * 4 +
         slots.capacity() * 4;
}

// --------------------------------------------------------------- Q2Slice

std::shared_ptr<Q2Slice> Q2Slice::Make(uint32_t uom, std::shared_ptr<const Q2Base> base) {
  auto s = std::make_shared<Q2Slice>();
  s->uom = uom;
  s->shard_shift = ShardShiftFor(base->n);
  const size_t shards = s->shard_shift >= 64 ? 1 : size_t{1} << (64 - s->shard_shift);
  s->shards.resize(shards);
  s->base = std::move(base);
  return s;
}

bool Q2Slice::Get(uint32_t adsh_id, uint32_t tag_id, Q2Group &out) const {
  const auto &shard = shards[ShardOf(Q2PairHash(adsh_id, tag_id))];
  if (shard) {
    const int32_t e = shard->Find(adsh_id, tag_id);
    if (e >= 0) {
      out = shard->ents[e];
      return out.c0 > 0;
    }
  }
  const int32_t pos = base->Find(adsh_id, tag_id);
  if (pos >= 0) {
    out = base->Get(static_cast<uint32_t>(pos), uom);
    return true;
  }
  return false;
}

std::vector<Q2Group> Q2Slice::LiveGroups() const {
  std::vector<Q2Group> groups;
  size_t xc = 0;
  for (uint32_t i = 0; i < base->n; ++i) {
    if (!IsDead(i)) {
      groups.push_back(base->GetSeq(i, uom, xc));
    }
  }
  for (const auto &shard : shards) {
    if (!shard) {
      continue;
    }
    for (const auto &g : shard->ents) {
      if (g.c0 > 0) {
        groups.push_back(g);
      }
    }
  }
  return groups;
}

uint64_t Q2Slice::LiveGroupCount() const {
  uint64_t live = base->n - dead_count;
  for (const auto &shard : shards) {
    if (!shard) {
      continue;
    }
    for (const auto &g : shard->ents) {
      live += g.c0 > 0 ? 1 : 0;
    }
  }
  return live;
}

bool Q2Slice::NeedsCompaction(uint64_t limit) const {
  if (limit == 0) {
    limit = AutoOverlayLimit(base->n);
  }
  return ov_count > limit;
}

std::shared_ptr<const Q2Base> Q2Slice::Compacted() const {
  std::vector<Q2Group> ov;
  ov.reserve(ov_count);
  for (const auto &shard : shards) {
    if (!shard) {
      continue;
    }
    for (const auto &g : shard->ents) {
      if (g.c0 > 0) {
        ov.push_back(g);
      }
    }
  }
  std::sort(ov.begin(), ov.end(), [](const Q2Group &a, const Q2Group &b) {
    if (a.k0 != b.k0) {
      return a.k0 > b.k0;
    }
    if (a.adsh != b.adsh) {
      return a.adsh < b.adsh;
    }
    return a.tag < b.tag;
  });
  auto out = std::make_shared<Q2Base>();
  const Q2Base &b = *base;
  out->Reserve(static_cast<size_t>(b.n - dead_count) + ov.size());
  size_t xc = 0;
  size_t o = 0;
  const bool any_dead = dead_count != 0;
  for (uint32_t i = 0; i < b.n; ++i) {
    if (any_dead && IsDead(i)) {
      continue;
    }
    const int64_t key = b.k0[i];
    while (o < ov.size() && ov[o].k0 > key) {
      out->PushBack(ov[o++]);
    }
    out->PushBack(b.GetSeq(i, uom, xc));
  }
  while (o < ov.size()) {
    out->PushBack(ov[o++]);
  }
  out->Finish();
  return out;
}

size_t Q2Slice::HeapBytes() const {
  size_t total = sizeof(*this) + shards.capacity() * sizeof(shards[0]) + dead.MemoryBytes();
  for (const auto &shard : shards) {
    if (shard) {
      total += shard->HeapBytes();
    }
  }
  if (base) {
    total += base->HeapBytes();
  }
  return total;
}

// ---------------------------------------------------------- Q2SliceWriter

Q2Slice &Q2SliceWriter::Writable(uint32_t uom) {
  if (owned_index_.size() <= uom) {
    owned_index_.resize(uom + 1, -1);
  }
  int32_t idx = owned_index_[uom];
  if (idx < 0) {
    if (state_.slices.size() <= uom) {
      state_.slices.resize(uom + 1);
    }
    std::shared_ptr<Q2Slice> copy;
    if (state_.slices[uom]) {
      copy = std::make_shared<Q2Slice>(*state_.slices[uom]); // shares shards / chunks
    } else {
      copy = Q2Slice::Make(uom, std::make_shared<const Q2Base>());
    }
    state_.slices[uom] = copy;
    idx = static_cast<int32_t>(owned_.size());
    owned_index_[uom] = idx;
    owned_.push_back({std::move(copy), std::vector<uint8_t>()});
    owned_.back().shard_owned.assign(owned_.back().slice->shards.size(), 0);
  }
  return *owned_[idx].slice;
}

void Q2SliceWriter::Put(const Q2Group &g, int32_t base_pos) {
  Q2Slice &s = Writable(g.uom);
  Owned &own = owned_[owned_index_[g.uom]];
  const size_t si = s.ShardOf(Q2PairHash(g.adsh, g.tag));
  auto &shard = s.shards[si];
  if (!own.shard_owned[si]) {
    shard = shard ? std::make_shared<Q2GroupTable>(*shard) : std::make_shared<Q2GroupTable>();
    own.shard_owned[si] = 1;
  }
  const int32_t e = shard->Find(g.adsh, g.tag);
  if (e >= 0) {
    shard->ents[e] = g;
    return;
  }
  const int32_t pos = base_pos != -2 ? base_pos : s.base->Find(g.adsh, g.tag);
  if (pos >= 0) {
    uint64_t &w = s.dead.Mutable(static_cast<int64_t>(static_cast<uint32_t>(pos) >> 6));
    const uint64_t bit = 1ull << (static_cast<uint32_t>(pos) & 63);
    if (!(w & bit)) {
      w |= bit;
      ++s.dead_count;
    }
  }
  shard->Append(g);
  ++s.ov_count;
}

unsigned Q2SliceWriter::CompactIfNeeded(uint64_t limit) {
  unsigned done = 0;
  for (auto &own : owned_) {
    Q2Slice &s = *own.slice;
    if (!s.NeedsCompaction(limit)) {
      continue;
    }
    auto base = s.Compacted();
    auto fresh = Q2Slice::Make(s.uom, std::move(base));
    state_.slices[s.uom] = fresh;
    own.slice = std::move(fresh);
    own.shard_owned.assign(own.slice->shards.size(), 1); // all empty and private
    ++done;
  }
  return done;
}

// ---------------------------------------------------------- SecEdgarQ2State

bool SecEdgarQ2State::Get(uint32_t uom, uint32_t adsh_id, uint32_t tag_id,
                          Q2Group &out) const {
  const Q2Slice *s = Slice(uom);
  return s && s->Get(adsh_id, tag_id, out);
}

std::vector<Q2Group> SecEdgarQ2State::LiveGroups(uint32_t uom) const {
  const Q2Slice *s = Slice(uom);
  return s ? s->LiveGroups() : std::vector<Q2Group>();
}

uint64_t SecEdgarQ2State::LiveGroupCount() const {
  uint64_t live = 0;
  for (const auto &s : slices) {
    if (s) {
      live += s->LiveGroupCount();
    }
  }
  return live;
}

void SecEdgarQ2State::SetSub(uint32_t adsh_id, bool fy_is_null, int64_t fy_value,
                             bool name_is_null, std::string_view name) {
  fy.Set(adsh_id, fy_is_null ? kQ2NoFy : fy_value);
  if (name_is_null) {
    names.Erase(adsh_id);
  } else {
    names.Set(adsh_id, name);
  }
}

void SecEdgarQ2State::ClearSub(uint32_t adsh_id) {
  if (fy.Has(adsh_id, kQ2NoFy)) {
    fy.Set(adsh_id, kQ2NoFy);
  }
  names.Erase(adsh_id);
}

size_t SecEdgarQ2State::HeapBytes() const {
  size_t total = sizeof(*this) + adsh.HeapBytes() + tag.HeapBytes() + fy.MemoryBytes() +
                 names.MemoryBytes() + slices.capacity() * sizeof(slices[0]);
  if (uoms) {
    total += uoms->HeapBytes();
  }
  for (const auto &s : slices) {
    if (s) {
      total += s->HeapBytes();
    }
  }
  return total;
}

} // namespace accelerator_codegen::aux_state::query
