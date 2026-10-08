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

int32_t Q2GroupTable::Find(uint32_t uom, uint32_t adsh_id, uint32_t tag_id) const {
  if (slots.empty()) {
    return -1;
  }
  const size_t mask = slots.size() - 1;
  size_t pos = Q2GroupHash(uom, adsh_id, tag_id) & mask;
  while (true) {
    const uint32_t v = slots[pos];
    if (v == 0) {
      return -1;
    }
    const Q2Group &g = ents[v - 1];
    if (g.uom == uom && g.adsh == adsh_id && g.tag == tag_id) {
      return static_cast<int32_t>(v - 1);
    }
    pos = (pos + 1) & mask;
  }
}

void Q2GroupTable::Put(const Q2Group &g) {
  const int32_t e = Find(g.uom, g.adsh, g.tag);
  if (e >= 0) {
    ents[e] = g;
    return;
  }
  if ((ents.size() + 1) * 2 > slots.size()) {
    const size_t cap = NextPow2((ents.size() + 1) * 4);
    slots.assign(cap, 0);
    const size_t mask = cap - 1;
    for (uint32_t i = 0; i < ents.size(); ++i) {
      size_t pos = Q2GroupHash(ents[i].uom, ents[i].adsh, ents[i].tag) & mask;
      while (slots[pos] != 0) {
        pos = (pos + 1) & mask;
      }
      slots[pos] = i + 1;
    }
  }
  ents.push_back(g);
  const size_t mask = slots.size() - 1;
  size_t pos = Q2GroupHash(g.uom, g.adsh, g.tag) & mask;
  while (slots[pos] != 0) {
    pos = (pos + 1) & mask;
  }
  slots[pos] = static_cast<uint32_t>(ents.size());
}

// --------------------------------------------------------------- Q2Base

std::shared_ptr<const Q2Base> Q2Base::Build(std::vector<Q2Group> &&groups,
                                            uint32_t uom_count) {
  if (groups.size() >= std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("sec_edgar q2: more than 2^32-2 materialised groups");
  }
  std::sort(groups.begin(), groups.end(), [](const Q2Group &a, const Q2Group &b) {
    if (a.uom != b.uom) {
      return a.uom < b.uom;
    }
    if (a.k0 != b.k0) {
      return a.k0 > b.k0;
    }
    if (a.adsh != b.adsh) {
      return a.adsh < b.adsh;
    }
    return a.tag < b.tag;
  });
  auto base = std::make_shared<Q2Base>();
  const uint32_t n = static_cast<uint32_t>(groups.size());
  base->n = n;
  base->k0.resize(n);
  base->k1.resize(n);
  base->adsh.resize(n);
  base->tag.resize(n);
  base->c0.resize(n);
  base->c1.resize(n);
  base->trunc.resize(n);
  base->uom_start.assign(uom_count + 1, 0);
  for (uint32_t i = 0; i < n; ++i) {
    const Q2Group &g = groups[i];
    base->k0[i] = g.k0;
    base->k1[i] = g.k1;
    base->adsh[i] = g.adsh;
    base->tag[i] = g.tag;
    base->c0[i] = g.c0;
    base->c1[i] = g.c1;
    base->trunc[i] = g.trunc ? 1 : 0;
    ++base->uom_start[g.uom + 1];
  }
  for (uint32_t u = 0; u < uom_count; ++u) {
    base->uom_start[u + 1] += base->uom_start[u];
  }
  const size_t cap = NextPow2(static_cast<size_t>(n) * 2);
  base->slots.assign(cap, 0);
  base->mask = static_cast<uint32_t>(cap - 1);
  for (uint32_t u = 0; u < uom_count; ++u) {
    for (uint32_t i = base->uom_start[u]; i < base->uom_start[u + 1]; ++i) {
      size_t pos = Q2GroupHash(u, base->adsh[i], base->tag[i]) & base->mask;
      while (base->slots[pos] != 0) {
        pos = (pos + 1) & base->mask;
      }
      base->slots[pos] = i + 1;
    }
  }
  return base;
}

int32_t Q2Base::Find(uint32_t uom, uint32_t adsh_id, uint32_t tag_id) const {
  const uint32_t begin = SliceBegin(uom);
  const uint32_t end = SliceEnd(uom);
  if (begin == end) {
    return -1;
  }
  size_t pos = Q2GroupHash(uom, adsh_id, tag_id) & mask;
  while (true) {
    const uint32_t v = slots[pos];
    if (v == 0) {
      return -1;
    }
    const uint32_t i = v - 1;
    if (i >= begin && i < end && adsh[i] == adsh_id && tag[i] == tag_id) {
      return static_cast<int32_t>(i);
    }
    pos = (pos + 1) & mask;
  }
}

Q2Group Q2Base::Get(uint32_t i, uint32_t uom) const {
  Q2Group g;
  g.k0 = k0[i];
  g.k1 = k1[i];
  g.uom = uom;
  g.adsh = adsh[i];
  g.tag = tag[i];
  g.c0 = c0[i];
  g.c1 = c1[i];
  g.trunc = trunc[i] != 0;
  return g;
}

size_t Q2Base::HeapBytes() const {
  return k0.capacity() * 8 + k1.capacity() * 8 + adsh.capacity() * 4 +
         tag.capacity() * 4 + c0.capacity() * 4 + c1.capacity() * 4 +
         trunc.capacity() + uom_start.capacity() * 4 + slots.capacity() * 4;
}

// ------------------------------------------------------------ Q2Overlay

void Q2Overlay::MarkDead(uint32_t pos) {
  uint64_t &w = dead.Mutable(static_cast<int64_t>(pos >> 6));
  const uint64_t bit = 1ull << (pos & 63);
  if (!(w & bit)) {
    w |= bit;
    ++dead_count;
  }
}

// ---------------------------------------------------------- SecEdgarQ2State

bool SecEdgarQ2State::Get(uint32_t uom, uint32_t adsh_id, uint32_t tag_id,
                          Q2Group &out) const {
  const int32_t e = ov.table.Find(uom, adsh_id, tag_id);
  if (e >= 0) {
    out = ov.table.ents[e];
    return out.c0 > 0;
  }
  const int32_t pos = base->Find(uom, adsh_id, tag_id);
  if (pos >= 0) {
    out = base->Get(static_cast<uint32_t>(pos), uom);
    return true;
  }
  return false;
}

void SecEdgarQ2State::Put(const Q2Group &g) {
  if (ov.table.Find(g.uom, g.adsh, g.tag) < 0) {
    const int32_t pos = base->Find(g.uom, g.adsh, g.tag);
    if (pos >= 0) {
      ov.MarkDead(static_cast<uint32_t>(pos));
    }
  }
  ov.table.Put(g);
}

bool SecEdgarQ2State::NeedsCompaction(size_t limit) const {
  // Automatic limit: a copy of the overlay costs O(limit) per write batch, a
  // compaction O(base); limit ~ 4 sqrt(base) balances the two.
  if (limit == 0) {
    limit = std::max<size_t>(512, 4 * IntSqrt(base->n));
  }
  return ov.table.ents.size() > limit;
}

std::vector<Q2Group> SecEdgarQ2State::LiveGroups(uint32_t uom) const {
  std::vector<Q2Group> groups;
  for (uint32_t i = base->SliceBegin(uom); i < base->SliceEnd(uom); ++i) {
    if (!ov.IsDead(i)) {
      groups.push_back(base->Get(i, uom));
    }
  }
  for (const auto &g : ov.table.ents) {
    if (g.uom == uom && g.c0 > 0) {
      groups.push_back(g);
    }
  }
  return groups;
}

void SecEdgarQ2State::Compact() {
  std::vector<Q2Group> groups;
  groups.reserve(base->n + ov.table.ents.size());
  for (uint32_t u = 0; u < base->UomCount(); ++u) {
    for (uint32_t i = base->uom_start[u]; i < base->uom_start[u + 1]; ++i) {
      if (!ov.IsDead(i)) {
        groups.push_back(base->Get(i, u));
      }
    }
  }
  for (const auto &g : ov.table.ents) {
    if (g.c0 > 0) {
      groups.push_back(g);
    }
  }
  base = Q2Base::Build(std::move(groups), static_cast<uint32_t>(uoms->names.size()));
  ov = Q2Overlay();
}

uint64_t SecEdgarQ2State::LiveGroupCount() const {
  uint64_t live = base->n - ov.dead_count;
  for (const auto &g : ov.table.ents) {
    live += g.c0 > 0 ? 1 : 0;
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
  size_t total = sizeof(*this) + ov.HeapBytes() + adsh.HeapBytes() + tag.HeapBytes() +
                 fy.MemoryBytes() + names.MemoryBytes();
  if (uoms) {
    total += uoms->HeapBytes();
  }
  if (base) {
    total += base->HeapBytes();
  }
  return total;
}

} // namespace accelerator_codegen::aux_state::query
