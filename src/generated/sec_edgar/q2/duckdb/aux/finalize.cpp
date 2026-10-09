#include "sec_edgar/q2/duckdb/aux/module.hpp"

#include <algorithm>

namespace accelerator_codegen::aux_state::query {
namespace {

using FyArray = decltype(SecEdgarQ2State::fy);

struct Cand {
  int64_t key;
  uint32_t adsh;
  uint32_t tag;
  uint32_t cnt;
  std::string_view name;
  bool has_name;
};

// A chunk of `fy` that no sub row wrote: every filer reads as "no fiscal year".
const int64_t *NoFyChunk() {
  static const std::vector<int64_t> chunk(FyArray::kChunkSize, kQ2NoFy);
  return chunk.data();
}

} // namespace

bool FinalizeSecEdgarQ2Views(const SecEdgarQ2State &state, int64_t fy,
                             std::string_view uom,
                             std::vector<SecEdgarQ2OutputView> &out) {
  out.clear();
  const Q2UomDir &dir = *state.uoms;
  const uint32_t ui = dir.names.Find(uom);
  if (ui == core::StringDict::kNone) {
    return true; // no non-NULL row ever carried this uom
  }
  if (dir.excluded[ui]) {
    return false;
  }
  if (fy == kQ2NoFy) {
    return true;
  }
  constexpr uint64_t kLimit = kSecEdgarQ2ResultLimit;
  constexpr unsigned kShift = 12;
  constexpr uint32_t kChunkMask = FyArray::kChunkSize - 1;

  // Resolve the fy array's chunks once so the scan below indexes them directly.
  thread_local std::vector<const int64_t *> fy_chunks;
  const size_t nchunks = (static_cast<size_t>(state.adsh.Size()) >> kShift) + 1;
  fy_chunks.resize(nchunks);
  for (size_t c = 0; c < nchunks; ++c) {
    const auto *chunk = state.fy.ChunkAt(c);
    fy_chunks[c] = chunk ? chunk->data() : NoFyChunk();
  }
  const int64_t *const *fy_of_chunk = fy_chunks.data();
  const auto fy_of = [&](uint32_t adsh) {
    return fy_of_chunk[adsh >> kShift][adsh & kChunkMask];
  };

  // Groups changed since the base was built.
  thread_local std::vector<Cand> ov_c;
  thread_local std::vector<Cand> cands;
  ov_c.clear();
  cands.clear();
  for (const auto &e : state.ov.table.ents) {
    if (e.uom == ui && e.c0 != 0 && fy_of(e.adsh) == fy) {
      ov_c.push_back({e.k0, e.adsh, e.tag, e.c0, {}, false});
    }
  }
  std::sort(ov_c.begin(), ov_c.end(),
            [](const Cand &a, const Cand &b) { return a.key > b.key; });

  uint64_t base_total = 0;
  {
    const Q2Base &b = *state.base;
    const uint32_t end = b.SliceEnd(ui);
    const int64_t *k0 = b.k0.data();
    const uint32_t *adsh = b.adsh.data();
    size_t ov_ptr = 0;
    uint64_t ov_above = 0;
    int64_t cur = 0;
    bool have_cur = false;
    bool done = false;
    const bool any_dead = state.ov.dead_count != 0;
    // Called for every matching (fy) base entry in descending key order.
    const auto accept = [&](uint32_t i) {
      if (any_dead && state.ov.IsDead(i)) {
        return;
      }
      const int64_t key = k0[i];
      if (!have_cur || key != cur) {
        cur = key;
        have_cur = true;
        while (ov_ptr < ov_c.size() && ov_c[ov_ptr].key > key) {
          ov_above += ov_c[ov_ptr].cnt;
          ++ov_ptr;
        }
        // kLimit rows with a strictly greater value already exist: nothing
        // from this tier on can reach the top-LIMIT.
        if (base_total + ov_above >= kLimit) {
          done = true;
          return;
        }
      }
      const uint32_t cnt = b.C0(i);
      cands.push_back({key, adsh[i], b.tag[i], cnt, {}, false});
      base_total += cnt;
    };
    // Note: an AVX2 gather variant of this loop was measured 2.5x slower than
    // the scalar loop on the target CPU (gathers are microcoded), so the scan
    // stays scalar.
    for (uint32_t i = b.SliceBegin(ui); i < end && !done; ++i) {
      if (fy_of(adsh[i]) == fy) {
        accept(i);
      }
    }
  }
  cands.insert(cands.end(), ov_c.begin(), ov_c.end());

  for (Cand &c : cands) {
    c.has_name = state.HasName(c.adsh);
    if (c.has_name) {
      c.name = state.Name(c.adsh);
    }
  }
  std::sort(cands.begin(), cands.end(), [&](const Cand &a, const Cand &b) {
    if (a.key != b.key) {
      return a.key > b.key;
    }
    if (a.adsh != b.adsh) {
      if (a.has_name != b.has_name) {
        return b.has_name ? false : true; // NULLS LAST
      }
      if (a.has_name) {
        const int nc = a.name.compare(b.name);
        if (nc != 0) {
          return nc < 0;
        }
      }
    }
    return state.tag.Get(a.tag).compare(state.tag.Get(b.tag)) < 0;
  });

  out.reserve(std::min<uint64_t>(kLimit, base_total + ov_c.size()));
  for (const Cand &c : cands) {
    for (uint32_t r = 0; r < c.cnt && out.size() < kLimit; ++r) {
      out.push_back({c.has_name ? c.name : std::string_view(), !c.has_name,
                     state.tag.Get(c.tag), Q2KeyValue(c.key)});
    }
    if (out.size() >= kLimit) {
      break;
    }
  }
  return true;
}

bool FinalizeSecEdgarQ2(const SecEdgarQ2State &state, int64_t fy,
                        std::string_view uom,
                        std::vector<SecEdgarQ2OutputRow> &out) {
  std::vector<SecEdgarQ2OutputView> views;
  const bool ok = FinalizeSecEdgarQ2Views(state, fy, uom, views);
  out.clear();
  for (const auto &v : views) {
    out.push_back({std::string(v.name), v.name_is_null, std::string(v.tag), v.value});
  }
  return ok;
}

} // namespace accelerator_codegen::aux_state::query
