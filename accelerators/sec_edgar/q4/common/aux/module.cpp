#include "sec_edgar/q4/common/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "sec_edgar/q4/common/aux/state_ops.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// Above these sizes a rebuild from the base tables is cheaper than patching.
constexpr size_t kMaxStructuralKeys = 4096;
constexpr size_t kMaxDeltas = 8u << 20;
// Rows per slice of a mutation's row loop (cell decoding and key copying).
constexpr size_t kAccumulateGrain = 4096;

enum class Cell { Ok, Null, Bad };

Cell GetString(const core::AuxRow &row, size_t index, std::string_view &out) {
  if (index >= row.values.size()) {
    return Cell::Bad;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return Cell::Null;
  }
  const auto *s = std::get_if<std::string>(&cell.payload);
  if (!s) {
    return Cell::Bad;
  }
  out = *s;
  return Cell::Ok;
}

Cell GetDouble(const core::AuxRow &row, size_t index, double &out) {
  if (index >= row.values.size()) {
    return Cell::Bad;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return Cell::Null;
  }
  if (const auto *d = std::get_if<double>(&cell.payload)) {
    out = *d;
    return Cell::Ok;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    out = static_cast<double>(*i);
    return Cell::Ok;
  }
  return Cell::Bad;
}

bool Intersects(const std::vector<core::ColumnId> &updated,
                std::initializer_list<core::ColumnId> watched) {
  if (updated.empty()) {
    return true; // unknown => assume relevant
  }
  for (const auto column : updated) {
    for (const auto w : watched) {
      if (column == w) return true;
    }
  }
  return false;
}

const SecEdgarQ4StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ4ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ4StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

std::shared_ptr<const SecEdgarQ4StateImage>
MakeImage(SecEdgarQ4ImageKind kind, const SecEdgarQ4State &full) {
  auto state = std::make_shared<SecEdgarQ4State>();
  switch (kind) {
  case SecEdgarQ4ImageKind::SicCohort:
    state->cohort = full.cohort;
    break;
  case SecEdgarQ4ImageKind::PreEqBridge:
    state->bridge = full.bridge;
    break;
  case SecEdgarQ4ImageKind::TagTlabel:
    state->labels = full.labels;
    break;
  case SecEdgarQ4ImageKind::GroupAggregates:
    state->groups = full.groups;
    break;
  }
  auto image = std::make_shared<SecEdgarQ4StateImage>();
  image->kind = kind;
  image->state = std::move(state);
  return image;
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

std::string Join(std::string_view a, std::string_view b) {
  std::string out;
  out.reserve(a.size() + b.size() + 1);
  out.append(a);
  out.push_back('\0');
  out.append(b);
  return out;
}

bool FilterMatches(const SecEdgarQ4BridgeFilter &f, const std::string &adsh,
                   const std::string &tag, const std::string &version,
                   const std::unordered_set<std::string> &adsh_set,
                   const std::unordered_set<std::string> &tv_set,
                   const std::unordered_set<std::string> &key_set) {
  if (f.all) return true;
  if (adsh_set.count(adsh)) return true;
  if (tv_set.count(Join(tag, version))) return true;
  if (key_set.count(Join(adsh, Join(tag, version)))) return true;
  return false;
}

// Integer-valued bound of a BETWEEN predicate.  Returns false when the
// argument is NULL / NaN (empty result).  Values beyond int32 are clamped: sic
// is an INTEGER column, so a clamped bound selects exactly the same rows.
bool ParseSicBound(const core::AuxValue &v, bool is_low, int32_t &out, bool &all_out_of_range) {
  all_out_of_range = false;
  if (v.is_null) return false;
  long double x;
  if (const auto *i = std::get_if<int64_t>(&v.payload)) {
    x = static_cast<long double>(*i);
  } else if (const auto *u = std::get_if<uint64_t>(&v.payload)) {
    x = static_cast<long double>(*u);
  } else if (const auto *d = std::get_if<double>(&v.payload)) {
    if (std::isnan(*d)) return false;
    x = *d;
    x = is_low ? std::ceil(x) : std::floor(x);
  } else if (const auto *str = std::get_if<std::string>(&v.payload)) {
    char *end = nullptr;
    errno = 0;
    const double d = std::strtod(str->c_str(), &end);
    if (end == str->c_str() || *end != '\0' || std::isnan(d)) return false;
    x = is_low ? std::ceil(d) : std::floor(d);
  } else {
    throw std::invalid_argument("sec_edgar q4 aux: unsupported sic argument");
  }
  constexpr long double kMin = -2147483648.0L, kMax = 2147483647.0L;
  if (is_low) {
    if (x > kMax) all_out_of_range = true;
    out = x < kMin ? INT32_MIN : (x > kMax ? INT32_MAX : static_cast<int32_t>(x));
  } else {
    if (x < kMin) all_out_of_range = true;
    out = x < kMin ? INT32_MIN : (x > kMax ? INT32_MAX : static_cast<int32_t>(x));
  }
  return true;
}

SecEdgarQ4Params ParseArguments(const core::QueryArguments &args) {
  SecEdgarQ4Params p;
  if (args.values.empty()) {
    return p;
  }
  if (args.values.size() != 4) {
    throw std::invalid_argument(
        "sec_edgar q4 aux expects () or (sic_lo INTEGER, sic_hi INTEGER, stmt VARCHAR, "
        "uom VARCHAR)");
  }
  bool lo_out = false, hi_out = false;
  const bool lo_ok = ParseSicBound(args.values[0], true, p.sic_lo, lo_out);
  const bool hi_ok = ParseSicBound(args.values[1], false, p.sic_hi, hi_out);
  const auto &stmt = args.values[2];
  const auto &uom = args.values[3];
  if (!lo_ok || !hi_ok || lo_out || hi_out || stmt.is_null || uom.is_null) {
    p.empty = true;
    return p;
  }
  if (!std::holds_alternative<std::string>(stmt.payload) ||
      !std::holds_alternative<std::string>(uom.payload)) {
    throw std::invalid_argument("sec_edgar q4 aux: stmt and uom must be strings");
  }
  p.stmt = std::get<std::string>(stmt.payload);
  p.uom = std::get<std::string>(uom.payload);
  if (p.sic_lo > p.sic_hi) p.empty = true;
  return p;
}

} // namespace

// ------------------------------------------------------------ default scan

void SecEdgarQ4InitialScan::ScanBridge(
    const SecEdgarQ4BridgeFilter &filter,
    const std::function<void(const SecEdgarQ4BridgeRow &)> &consumer) const {
  std::unordered_set<std::string> adsh_set(filter.adsh.begin(), filter.adsh.end());
  std::unordered_set<std::string> tv_set;
  std::unordered_set<std::string> key_set;
  for (const auto &tv : filter.tag_versions) tv_set.insert(Join(tv.first, tv.second));
  for (const auto &k : filter.keys) key_set.insert(Join(k.adsh, Join(k.tag, k.version)));

  struct SubInfo {
    int32_t cik;
    int32_t sic;
    bool cik_null;
  };
  std::unordered_map<std::string, SubInfo> subs;
  ScanSub([&](const SecEdgarQ4SubInput &s) { subs[s.adsh] = {s.cik, s.sic, s.cik_null}; });
  struct TagInfo {
    std::string tlabel;
    bool tlabel_null;
  };
  std::unordered_map<std::string, TagInfo> tags; // tag\0version -> label
  ScanTag([&](const SecEdgarQ4TagInput &t) {
    if (t.abstract_flag == 0) {
      tags[Join(t.tag, t.version)] = {t.tlabel, t.tlabel_null};
    }
  });
  struct KeyAcc {
    std::string adsh, tag, version;
    SubInfo sub;
    TagInfo tag_info;
    std::map<std::string, uint32_t> stmts;
    struct Agg {
      double hi = 0.0, lo = 0.0;
      int64_t nc = 0;
    };
    std::map<std::string, Agg> uoms;
  };
  std::unordered_map<std::string, KeyAcc> keys; // adsh\0tag\0version
  ScanPre([&](const SecEdgarQ4PreInput &p) {
    const auto sub = subs.find(p.adsh);
    if (sub == subs.end()) return;
    const auto tag = tags.find(Join(p.tag, p.version));
    if (tag == tags.end()) return;
    if (!FilterMatches(filter, p.adsh, p.tag, p.version, adsh_set, tv_set, key_set)) return;
    auto &acc = keys[Join(p.adsh, Join(p.tag, p.version))];
    if (acc.stmts.empty()) {
      acc.adsh = p.adsh;
      acc.tag = p.tag;
      acc.version = p.version;
      acc.sub = sub->second;
      acc.tag_info = tag->second;
    }
    ++acc.stmts[p.stmt];
  });
  ScanNum([&](const SecEdgarQ4NumInput &n) {
    if (n.value_is_null) return;
    const auto it = keys.find(Join(n.adsh, Join(n.tag, n.version)));
    if (it == keys.end()) return;
    auto &agg = it->second.uoms[n.uom];
    Q4DDAdd(agg.hi, agg.lo, n.value);
    agg.nc += 1;
  });
  for (const auto &kv : keys) {
    const auto &acc = kv.second;
    for (const auto &st : acc.stmts) {
      SecEdgarQ4BridgeRow row;
      row.adsh = acc.adsh;
      row.tag = acc.tag;
      row.version = acc.version;
      row.stmt = st.first;
      row.mult = st.second;
      row.cik = acc.sub.cik;
      row.cik_null = acc.sub.cik_null;
      row.sic = acc.sub.sic;
      row.tlabel = acc.tag_info.tlabel;
      row.tlabel_null = acc.tag_info.tlabel_null;
      if (acc.uoms.empty()) {
        consumer(row);
        continue;
      }
      for (const auto &u : acc.uoms) {
        row.has_num = true;
        row.uom = u.first;
        row.ns = u.second.hi;
        row.ns_lo = u.second.lo;
        row.nc = u.second.nc;
        consumer(row);
      }
    }
  }
}

uint64_t SecEdgarQ4ResidentBytes(const SecEdgarQ4State &state, SecEdgarQ4ImageKind kind) {
  switch (kind) {
  case SecEdgarQ4ImageKind::SicCohort:
    return state.cohort ? state.cohort->Bytes() : 0;
  case SecEdgarQ4ImageKind::PreEqBridge:
    return state.bridge ? state.bridge->Bytes() : 0;
  case SecEdgarQ4ImageKind::TagTlabel:
    return state.labels ? state.labels->Bytes() : 0;
  case SecEdgarQ4ImageKind::GroupAggregates:
    return state.groups ? state.groups->Bytes() + (state.groups->sud ? state.groups->sud->Bytes() : 0)
                        : 0;
  }
  return 0;
}

void SecEdgarQ4InitialScan::ScanDirty(const SecEdgarQ4BridgeFilter &filter,
                                      SecEdgarQ4DirtyRows &out) const {
  std::unordered_set<std::string> adsh_set(filter.adsh.begin(), filter.adsh.end());
  std::unordered_set<std::string> tv_set;
  std::unordered_set<std::string> key_set;
  for (const auto &tv : filter.tag_versions) tv_set.insert(Join(tv.first, tv.second));
  for (const auto &k : filter.keys) key_set.insert(Join(k.adsh, Join(k.tag, k.version)));
  std::unordered_set<std::string> need_adsh(adsh_set), need_tv(tv_set);
  for (const auto &k : filter.keys) {
    need_adsh.insert(k.adsh);
    need_tv.insert(Join(k.tag, k.version));
  }
  const auto match = [&](const std::string &a, const std::string &t, const std::string &v) {
    if (filter.all) return true;
    return adsh_set.count(a) != 0 || tv_set.count(Join(t, v)) != 0 ||
           key_set.count(Join(a, Join(t, v))) != 0;
  };
  ScanPre([&](const SecEdgarQ4PreInput &p) {
    if (!match(p.adsh, p.tag, p.version)) return;
    out.pres.push_back(p);
    need_adsh.insert(p.adsh);
    need_tv.insert(Join(p.tag, p.version));
  });
  ScanNum([&](const SecEdgarQ4NumInput &n) {
    if (n.value_is_null || !match(n.adsh, n.tag, n.version)) return;
    out.nums.push_back(n);
  });
  ScanSub([&](const SecEdgarQ4SubInput &s) {
    if (filter.all || need_adsh.count(s.adsh)) out.subs.push_back(s);
  });
  ScanTag([&](const SecEdgarQ4TagInput &t) {
    if (filter.all || need_tv.count(Join(t.tag, t.version))) out.tags.push_back(t);
  });
}

// ------------------------------------------------------------- txn buffer

bool SecEdgarQ4TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ4TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  arena.clear();
  deltas.clear();
  dirty_adsh.clear();
  dirty_tv.clear();
  dirty_keys.clear();
}

bool SecEdgarQ4TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

void SecEdgarQ4TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  arena.clear();
  deltas.clear();
  dirty_adsh.clear();
  dirty_tv.clear();
  dirty_keys.clear();
}

void SecEdgarQ4TxnBuffer::AddNumDelta(std::string_view adsh, std::string_view tag,
                                      std::string_view version, std::string_view uom,
                                      double value, int8_t sign) {
  if (requires_rebuild) return;
  pending = true;
  NumDelta d;
  d.off = arena.size();
  d.adsh_len = static_cast<uint32_t>(adsh.size());
  d.tag_len = static_cast<uint32_t>(tag.size());
  d.version_len = static_cast<uint32_t>(version.size());
  d.uom_len = static_cast<uint32_t>(uom.size());
  d.value = value;
  d.sign = sign;
  arena.append(adsh);
  arena.append(tag);
  arena.append(version);
  arena.append(uom);
  deltas.push_back(d);
}

void SecEdgarQ4TxnBuffer::AddNumUpdate(std::string_view adsh, std::string_view tag,
                                       std::string_view version, std::string_view uom,
                                       double old_value, double new_value) {
  AddNumDelta(adsh, tag, version, uom, old_value, 0);
  if (!requires_rebuild) deltas.back().value_new = new_value;
}

void SecEdgarQ4TxnBuffer::Absorb(SecEdgarQ4TxnBuffer &&from) {
  if (requires_rebuild) return;
  if (from.requires_rebuild) {
    MarkRebuild();
    return;
  }
  pending = pending || from.pending;
  const uint64_t shift = arena.size();
  arena.append(from.arena);
  for (auto d : from.deltas) {
    d.off += shift;
    deltas.push_back(d);
  }
  const auto append = [](auto &to, auto &&source) {
    to.insert(to.end(), std::make_move_iterator(source.begin()),
              std::make_move_iterator(source.end()));
  };
  append(dirty_adsh, std::move(from.dirty_adsh));
  append(dirty_tv, std::move(from.dirty_tv));
  append(dirty_keys, std::move(from.dirty_keys));
}

void SecEdgarQ4TxnBuffer::AddDirtyAdsh(std::string adsh) {
  if (requires_rebuild) return;
  pending = true;
  dirty_adsh.push_back(std::move(adsh));
}

void SecEdgarQ4TxnBuffer::AddDirtyTagVersion(std::string tag, std::string version) {
  if (requires_rebuild) return;
  pending = true;
  dirty_tv.emplace_back(std::move(tag), std::move(version));
}

void SecEdgarQ4TxnBuffer::AddDirtyKey(std::string adsh, std::string tag,
                                      std::string version) {
  if (requires_rebuild) return;
  pending = true;
  dirty_keys.push_back({std::move(adsh), std::move(tag), std::move(version)});
}

// ------------------------------------------------------------------ module

core::AcceleratorId SecEdgarQ4Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ4Module::Dependencies() const {
  return {{config.num_table, {kQ4NumAdsh, kQ4NumTag, kQ4NumVersion, kQ4NumUom, kQ4NumValue}},
          {config.sub_table, {kQ4SubAdsh, kQ4SubCik, kQ4SubSic}},
          {config.tag_table, {kQ4TagTag, kQ4TagVersion, kQ4TagAbstract, kQ4TagTlabel}},
          {config.pre_table, {kQ4PreAdsh, kQ4PreStmt, kQ4PreTag, kQ4PreVersion}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ4Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ4TxnBuffer>();
}

std::vector<core::StateImage>
SecEdgarQ4Module::MakeImages(const SecEdgarQ4State &state,
                             const core::Version versions[4]) const {
  const auto id = Id();
  std::vector<core::StateImage> images;
  images.push_back(core::StateImage::Make(
      id, core::SecEdgarQ4Layout::kSicCohort, versions[0],
      MakeImage(SecEdgarQ4ImageKind::SicCohort, state)));
  images.push_back(core::StateImage::Make(
      id, core::SecEdgarQ4Layout::kPreEqBridge, versions[1],
      MakeImage(SecEdgarQ4ImageKind::PreEqBridge, state)));
  images.push_back(core::StateImage::Make(
      id, core::SecEdgarQ4Layout::kTagTlabel, versions[2],
      MakeImage(SecEdgarQ4ImageKind::TagTlabel, state)));
  images.push_back(core::StateImage::Make(
      id, core::SecEdgarQ4Layout::kGroupAggregates, versions[3],
      MakeImage(SecEdgarQ4ImageKind::GroupAggregates, state)));
  return images;
}

void SecEdgarQ4Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const core::Version versions[4] = {0, 0, 0, 0};
  for (auto &image : MakeImages(state, versions)) {
    builder.Write(std::move(image));
  }
  builder.Finish();
}

SecEdgarQ4State SecEdgarQ4Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q4 requires a typed initial scan source");
  }
  q4::Builder builder;
  if (!initial_scan->BuildFull(builder)) {
    SecEdgarQ4BridgeFilter filter;
    initial_scan->ScanBridge(filter, [&](const SecEdgarQ4BridgeRow &row) { builder.AddBridgeRow(row); });
  }
  return builder.Finish(services);
}

core::InvalidationPlan
SecEdgarQ4Module::InterpretMutation(const core::MutationBatch &) const {
  // All relevant mutations are staged incrementally through the txn buffer
  // (or force a rebuild from there); nothing needs eager invalidation.
  return {};
}

void SecEdgarQ4Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &services,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ4TxnBuffer &>(base_buffer);
  const bool is_update = mutation.kind == core::MutationKind::Update;
  if (is_update && mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }

  // Runs body(out, begin, end) over [0, n) in slices, each buffering into its
  // own SecEdgarQ4TxnBuffer that is appended to `buffer` in slice order.
  const auto reduce = [&](std::size_t n, auto &&body) {
    core::ParallelReduce(services, n, kAccumulateGrain, buffer, body,
                         [](SecEdgarQ4TxnBuffer &into, SecEdgarQ4TxnBuffer &&from) {
                           into.Absorb(std::move(from));
                         });
  };

  // Row accessors per mutation kind: each (row, sign) pair is one table row
  // that disappears (-1, old image) or appears (+1, new image).
  std::vector<std::pair<const core::AuxRow *, int8_t>> rows;
  if (mutation.kind != core::MutationKind::Insert) {
    for (const auto &r : mutation.old_rows) rows.emplace_back(&r, int8_t(-1));
  }
  if (mutation.kind != core::MutationKind::Delete) {
    for (const auto &r : mutation.new_rows) rows.emplace_back(&r, int8_t(+1));
  }

  if (mutation.table == config.num_table) {
    if (is_update &&
        !Intersects(mutation.updated_columns,
                    {kQ4NumAdsh, kQ4NumTag, kQ4NumVersion, kQ4NumUom, kQ4NumValue})) {
      return;
    }
    struct NumCells {
      std::string_view adsh, tag, version, uom;
      double value = 0.0;
      bool usable = false; // every column non-NULL
    };
    // false => the row is malformed (rebuild)
    const auto read = [&](const core::AuxRow &row, NumCells &c) {
      const Cell a = GetString(row, kQ4NumAdsh, c.adsh);
      const Cell t = GetString(row, kQ4NumTag, c.tag);
      const Cell v = GetString(row, kQ4NumVersion, c.version);
      const Cell u = GetString(row, kQ4NumUom, c.uom);
      const Cell x = GetDouble(row, kQ4NumValue, c.value);
      if (a == Cell::Bad || t == Cell::Bad || v == Cell::Bad || u == Cell::Bad || x == Cell::Bad) {
        return false;
      }
      c.usable = a == Cell::Ok && t == Cell::Ok && v == Cell::Ok && u == Cell::Ok && x == Cell::Ok;
      return !c.usable || std::isfinite(c.value); // inf/NaN cannot be subtracted back out
    };
    if (is_update) {
      // An UPDATE row keeps its key in the common case (value changes): one
      // paired delta, one copy of the key.
      reduce(mutation.new_rows.size(), [&](SecEdgarQ4TxnBuffer &out, size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
          NumCells o, n;
          if (!read(mutation.old_rows[i], o) || !read(mutation.new_rows[i], n)) {
            out.MarkRebuild();
            return;
          }
          if (o.usable && n.usable && o.adsh == n.adsh && o.tag == n.tag &&
              o.version == n.version && o.uom == n.uom) {
            out.AddNumUpdate(o.adsh, o.tag, o.version, o.uom, o.value, n.value);
            continue;
          }
          if (o.usable) out.AddNumDelta(o.adsh, o.tag, o.version, o.uom, o.value, -1);
          if (n.usable) out.AddNumDelta(n.adsh, n.tag, n.version, n.uom, n.value, +1);
        }
      });
      return;
    }
    reduce(rows.size(), [&](SecEdgarQ4TxnBuffer &out, size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        const auto &[row, sign] = rows[i];
        NumCells c;
        if (!read(*row, c)) {
          out.MarkRebuild();
          return;
        }
        if (c.usable) out.AddNumDelta(c.adsh, c.tag, c.version, c.uom, c.value, sign);
      }
    });
    return;
  }

  if (mutation.table == config.sub_table) {
    if (is_update && !Intersects(mutation.updated_columns, {kQ4SubAdsh, kQ4SubCik, kQ4SubSic})) {
      return;
    }
    reduce(rows.size(), [&](SecEdgarQ4TxnBuffer &out, size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        std::string_view adsh;
        const Cell a = GetString(*rows[i].first, kQ4SubAdsh, adsh);
        if (a == Cell::Bad) {
          out.MarkRebuild();
          return;
        }
        if (a == Cell::Ok) out.AddDirtyAdsh(std::string(adsh));
      }
    });
    return;
  }

  if (mutation.table == config.tag_table) {
    if (is_update && !Intersects(mutation.updated_columns,
                                 {kQ4TagTag, kQ4TagVersion, kQ4TagAbstract, kQ4TagTlabel})) {
      return;
    }
    reduce(rows.size(), [&](SecEdgarQ4TxnBuffer &out, size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        std::string_view tag, version;
        const Cell t = GetString(*rows[i].first, kQ4TagTag, tag);
        const Cell v = GetString(*rows[i].first, kQ4TagVersion, version);
        if (t == Cell::Bad || v == Cell::Bad) {
          out.MarkRebuild();
          return;
        }
        if (t == Cell::Ok && v == Cell::Ok) {
          out.AddDirtyTagVersion(std::string(tag), std::string(version));
        }
      }
    });
    return;
  }

  if (mutation.table == config.pre_table) {
    if (is_update && !Intersects(mutation.updated_columns,
                                 {kQ4PreAdsh, kQ4PreStmt, kQ4PreTag, kQ4PreVersion})) {
      return;
    }
    reduce(rows.size(), [&](SecEdgarQ4TxnBuffer &out, size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        const core::AuxRow &row = *rows[i].first;
        std::string_view adsh, stmt, tag, version;
        const Cell a = GetString(row, kQ4PreAdsh, adsh);
        const Cell s = GetString(row, kQ4PreStmt, stmt);
        const Cell t = GetString(row, kQ4PreTag, tag);
        const Cell v = GetString(row, kQ4PreVersion, version);
        if (a == Cell::Bad || s == Cell::Bad || t == Cell::Bad || v == Cell::Bad) {
          out.MarkRebuild();
          return;
        }
        if (a != Cell::Ok || t != Cell::Ok || v != Cell::Ok) continue; // cannot join
        if (s == Cell::Null) continue; // p.stmt = :stmt never holds for NULL
        out.AddDirtyKey(std::string(adsh), std::string(tag), std::string(version));
      }
    });
    return;
  }
}

core::StagedMaintenanceOutcome
SecEdgarQ4Module::RebuildOutcome(core::AuxEngineServices &services, const char *why) const {
  (void)why;
  core::StagedMaintenanceOutcome outcome;
  const auto decision = services.ChooseMaintenance(
      {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                       ? core::MaintenanceAction::RebuildAll
                       : decision.action;
  outcome.invalidations = AllPartitions();
  outcome.pending_rebuild = outcome.invalidations;
  return outcome;
}

bool SecEdgarQ4Module::ReadState(core::AuxEngineServices &services,
                                 SecEdgarQ4State &state,
                                 core::Version versions[4]) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ4Layout::kSicCohort, core::SecEdgarQ4Layout::kPreEqBridge,
       core::SecEdgarQ4Layout::kTagTlabel, core::SecEdgarQ4Layout::kGroupAggregates});
  bool have = false;
  for (const auto &image : snapshot.partitions) {
    switch (image.partition) {
    case core::SecEdgarQ4Layout::kSicCohort:
      versions[0] = image.version;
      break;
    case core::SecEdgarQ4Layout::kPreEqBridge:
      versions[1] = image.version;
      break;
    case core::SecEdgarQ4Layout::kTagTlabel:
      versions[2] = image.version;
      break;
    case core::SecEdgarQ4Layout::kGroupAggregates:
      if (const auto *t = AsImage(image, SecEdgarQ4ImageKind::GroupAggregates)) {
        // The groups of a version carry the cohort / bridge / labels they were
        // built with, so this one image is a consistent snapshot.
        const auto &g = t->state->groups;
        if (g->cohort && g->bridge && g->labels && g->sud && g->su_slot && g->pairs && g->gindex) {
          state.groups = g;
          state.cohort = g->cohort;
          state.bridge = g->bridge;
          state.labels = g->labels;
          versions[3] = image.version;
          have = true;
        }
      }
      break;
    default:
      break;
    }
  }
  return have;
}

core::StagedMaintenanceOutcome
SecEdgarQ4Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ4TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    return RebuildOutcome(services, "buffer marked rebuild");
  }
  if (!buffer.HasStructural() && buffer.Deltas().empty()) {
    return outcome; // nothing relevant changed
  }

  SecEdgarQ4State state;
  core::Version versions[4] = {0, 0, 0, 0};
  if (!ReadState(services, state, versions) || buffer.Deltas().size() > kMaxDeltas) {
    return RebuildOutcome(services, "state not readable / too many deltas");
  }

  const bool structural = buffer.HasStructural();
  std::unordered_set<std::string> structural_keys;
  try {
    q4::Writer writer(state);
    if (structural) {
      auto &da = buffer.dirty_adsh;
      std::sort(da.begin(), da.end());
      da.erase(std::unique(da.begin(), da.end()), da.end());
      auto &dt = buffer.dirty_tv;
      std::sort(dt.begin(), dt.end());
      dt.erase(std::unique(dt.begin(), dt.end()), dt.end());
      auto &dk = buffer.dirty_keys;
      std::sort(dk.begin(), dk.end(), [](const auto &a, const auto &b) {
        return std::tie(a.adsh, a.tag, a.version) < std::tie(b.adsh, b.tag, b.version);
      });
      dk.erase(std::unique(dk.begin(), dk.end(),
                           [](const auto &a, const auto &b) {
                             return a.adsh == b.adsh && a.tag == b.tag && a.version == b.version;
                           }),
               dk.end());
      if (!initial_scan || da.size() + dt.size() + dk.size() > kMaxStructuralKeys) {
        return RebuildOutcome(services, "too many structural keys or no scan");
      }
      SecEdgarQ4BridgeFilter filter;
      filter.all = false;
      filter.adsh = da;
      filter.tag_versions = dt;
      filter.keys = dk;
      SecEdgarQ4DirtyRows rows;
      initial_scan->ScanDirty(filter, rows);
      // Remove the old contribution of every affected key, add the current one.
      if (!writer.ApplyStructural(rows, buffer, structural_keys)) {
        return RebuildOutcome(services, "structural apply failed");
      }
    }
    // The num deltas of the structurally re-read keys are already reflected.
      if (!writer.ApplyNumDeltas(services, buffer, structural ? &structural_keys : nullptr)) {
      return RebuildOutcome(services, "delta inconsistent with state");
    }
    const bool full = writer.StructureChanged();
    state = writer.Finish();
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.expected_version = 0;
    if (full) {
      outcome.updates = MakeImages(state, versions);
    } else {
      outcome.updates.push_back(core::StateImage::Make(
          Id(), core::SecEdgarQ4Layout::kGroupAggregates, versions[3],
          MakeImage(SecEdgarQ4ImageKind::GroupAggregates, state)));
    }
  } catch (const std::exception &) {
    return RebuildOutcome(services, "apply failed");
  }
  return outcome;
}

bool SecEdgarQ4Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ4Module::EmitGroups(const core::AuxEngineServices &services,
                                  const SecEdgarQ4Groups &groups,
                                  const SecEdgarQ4Params &params,
                                  core::AuxRowSink &sink) const {
  const auto result = q4::CachedResult(services, groups, params);
  sink.Begin({{{"sic", core::AuxType::Int64, false},
               {"tlabel", core::AuxType::String, true},
               {"stmt", core::AuxType::String, false},
               {"num_companies", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false},
               {"avg_value", core::AuxType::Double, false}}});
  // One row object reused for every output row (no per-row allocation).
  core::AuxRow out;
  out.values.reserve(6);
  out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{0}));
  out.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string()));
  out.values.push_back(core::AuxValue::Of(core::AuxType::String, params.stmt));
  out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{0}));
  out.values.push_back(core::AuxValue::Of(core::AuxType::Double, 0.0));
  out.values.push_back(core::AuxValue::Of(core::AuxType::Double, 0.0));
  const auto &tls = groups.labels->tlabels;
  for (const auto &row : result->rows) {
    std::get<int64_t>(out.values[0].payload) = row.sic;
    if (row.tl == kQ4None) {
      out.values[1].is_null = true;
    } else {
      out.values[1].is_null = false;
      std::get<std::string>(out.values[1].payload).assign(tls.Get(row.tl));
    }
    std::get<int64_t>(out.values[3].payload) = row.distinct;
    std::get<double>(out.values[4].payload) = row.total;
    std::get<double>(out.values[5].payload) = row.avg;
    sink.WriteRow(out);
  }
  sink.End();
}

void SecEdgarQ4Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const SecEdgarQ4Params params = ParseArguments(arguments);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ4Layout::kGroupAggregates});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::SecEdgarQ4Layout::kGroupAggregates) continue;
    const auto *typed = AsImage(image, SecEdgarQ4ImageKind::GroupAggregates);
    if (typed && typed->state->groups) {
      EmitGroups(services, *typed->state->groups, params, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void SecEdgarQ4Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  if (services.AnswerFromSql(arguments, sink)) {
    return;
  }
  const SecEdgarQ4Params params = ParseArguments(arguments);
  const auto state = BuildState(services);
  EmitGroups(services, *state.groups, params, sink);
}

} // namespace accelerator_codegen::aux_state::query
