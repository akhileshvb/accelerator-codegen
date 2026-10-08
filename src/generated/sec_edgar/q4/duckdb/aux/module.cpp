#include "sec_edgar/q4/duckdb/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/stateful/core/byte_stream.hpp"
#include "sec_edgar/q4/duckdb/aux/state_ops.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteWriter;

constexpr uint32_t kFormatVersion = 2;
constexpr uint32_t kSicCohortMagic = 0x43345151;      // Q4SC
constexpr uint32_t kPreEqBridgeMagic = 0x50345151;    // Q4PB
constexpr uint32_t kTagTlabelMagic = 0x54345151;      // Q4TT
constexpr uint32_t kGroupAggregateMagic = 0x41345151; // Q4AG
constexpr int32_t kSicMin = 4000;
constexpr int32_t kSicMax = 4999;
// Above these sizes a rebuild from the base tables is cheaper than patching.
constexpr size_t kMaxStructuralKeys = 4096;
constexpr size_t kMaxDeltas = 8u << 20;

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

void PutVar(ByteWriter &w, uint64_t v) {
  while (v >= 0x80) {
    w.U8(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  w.U8(static_cast<uint8_t>(v));
}

void PutDict(ByteWriter &w, const SecEdgarQ4StrDict &dict) {
  PutVar(w, dict.Size());
  for (uint32_t i = 0; i < dict.Size(); ++i) {
    const auto s = dict.Get(i);
    PutVar(w, s.size());
    w.bytes.insert(w.bytes.end(), s.begin(), s.end());
  }
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
  ScanSub([&](const SecEdgarQ4SubInput &s) {
    if (s.sic >= kSicMin && s.sic <= kSicMax) {
      subs[s.adsh] = {s.cik, s.sic, s.cik_null};
    }
  });
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
  struct Acc {
    SecEdgarQ4BridgeRow row;
  };
  std::unordered_map<std::string, SecEdgarQ4BridgeRow> keys; // adsh\0tag\0version
  ScanPre([&](const SecEdgarQ4PreInput &p) {
    if (p.stmt != "EQ") return;
    const auto sub = subs.find(p.adsh);
    if (sub == subs.end()) return;
    const auto tag = tags.find(Join(p.tag, p.version));
    if (tag == tags.end()) return;
    if (!FilterMatches(filter, p.adsh, p.tag, p.version, adsh_set, tv_set, key_set)) return;
    auto &row = keys[Join(p.adsh, Join(p.tag, p.version))];
    if (row.mult == 0) {
      row.adsh = p.adsh;
      row.tag = p.tag;
      row.version = p.version;
      row.cik = sub->second.cik;
      row.cik_null = sub->second.cik_null;
      row.sic = sub->second.sic;
      row.tlabel = tag->second.tlabel;
      row.tlabel_null = tag->second.tlabel_null;
    }
    ++row.mult;
  });
  if (filter.include_num) {
    ScanNum([&](const SecEdgarQ4NumInput &n) {
      if (n.uom != "USD" || n.value_is_null) return;
      const auto it = keys.find(Join(n.adsh, Join(n.tag, n.version)));
      if (it == keys.end()) return;
      Q4DDAdd(it->second.ns, it->second.ns_lo, n.value);
      it->second.nc += 1;
    });
  }
  for (const auto &kv : keys) {
    consumer(kv.second);
  }
}

// -------------------------------------------------------------- state build

SecEdgarQ4State SecEdgarQ4StateFromBridgeRows(
    const std::vector<SecEdgarQ4BridgeRow> &rows) {
  q4::Flat flat;
  for (const auto &row : rows) {
    flat.AddRow(row);
  }
  return q4::FlatToState(flat, 1);
}

uint32_t SecEdgarQ4FindEntry(const SecEdgarQ4State &state, std::string_view adsh,
                             std::string_view tag, std::string_view version) {
  if (!state.cohort || !state.bridge) return kQ4None;
  const uint32_t a = state.cohort->adsh.Find(adsh);
  if (a == kQ4None) return kQ4None;
  const uint32_t tv = state.bridge->tv_dict->Find(Join(tag, version));
  if (tv == kQ4None) return kQ4None;
  return state.bridge->Lookup(a, tv);
}

std::vector<SecEdgarQ4OutputRow> BuildSecEdgarQ4Output(const SecEdgarQ4State &state) {
  std::vector<SecEdgarQ4OutputRow> out;
  if (!state.groups) return out;
  const auto result = q4::ComputeResult(*state.groups);
  for (const auto &r : result->rows) {
    SecEdgarQ4OutputRow row;
    row.sic = r.sic;
    row.tlabel_null = r.tl == kQ4None;
    if (!row.tlabel_null) row.tlabel = std::string(state.groups->labels->tlabels.Get(r.tl));
    row.num_companies = r.distinct;
    row.total_value = r.total;
    row.avg_value = r.avg;
    out.push_back(std::move(row));
  }
  return out;
}

// ---------------------------------------------------------------- encodings

std::vector<uint8_t> EncodeSecEdgarQ4SicCohort(const SecEdgarQ4State &state) {
  ByteWriter w;
  const auto &c = *state.cohort;
  core::WriteImageHeader(w, kSicCohortMagic, kFormatVersion, c.adsh.Size());
  PutDict(w, c.adsh);
  PutVar(w, c.ciks.size());
  for (const auto cik : c.ciks) w.I32(cik);
  for (const auto id : c.cik_id) PutVar(w, id == kQ4None ? 0 : id + 1ull);
  return std::move(w.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ4PreEqBridge(const SecEdgarQ4State &state) {
  ByteWriter w;
  const auto &b = *state.bridge;
  core::WriteImageHeader(w, kPreEqBridgeMagic, kFormatVersion, b.entry_count);
  PutDict(w, *b.tv_dict);
  for (const auto tl : *b.tv_tl) PutVar(w, tl == kQ4None ? 0 : tl + 1ull);
  for (uint32_t id = 0; id < b.entry_count; ++id) {
    const auto &e = b.At(id);
    PutVar(w, e.adsh_id);
    PutVar(w, e.tv_id);
    PutVar(w, e.group);
    PutVar(w, e.mult);
    PutVar(w, static_cast<uint64_t>(e.nc));
    if (e.nc != 0) {
      w.Double(e.ns_hi);
      w.U8(e.ns_lo != 0.0);
      if (e.ns_lo != 0.0) w.Double(e.ns_lo);
    }
  }
  return std::move(w.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ4TagTlabel(const SecEdgarQ4State &state) {
  ByteWriter w;
  core::WriteImageHeader(w, kTagTlabelMagic, kFormatVersion, state.labels->tlabels.Size());
  PutDict(w, state.labels->tlabels);
  return std::move(w.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ4GroupAggregates(const SecEdgarQ4State &state) {
  ByteWriter w;
  const auto &g = *state.groups;
  core::WriteImageHeader(w, kGroupAggregateMagic, kFormatVersion, g.group_count);
  for (uint32_t id = 0; id < g.group_count; ++id) {
    const auto &sh = *g.gshards[id / kQ4GroupShardSize];
    const uint32_t i = id % kQ4GroupShardSize;
    PutVar(w, static_cast<uint64_t>(sh.sic[i]));
    PutVar(w, sh.tl[i] == kQ4None ? 0 : sh.tl[i] + 1ull);
    PutVar(w, static_cast<uint64_t>(sh.count[i]));
    PutVar(w, sh.distinct[i]);
    if (sh.count[i] != 0) {
      w.Double(sh.sum_hi[i]);
      w.U8(sh.sum_lo[i] != 0.0);
      if (sh.sum_lo[i] != 0.0) w.Double(sh.sum_lo[i]);
    }
  }
  uint32_t pairs = 0;
  g.pairs.ForEach([&](int64_t, const int64_t &count) { pairs += count != 0; });
  w.U32(pairs);
  g.pairs.ForEach([&](int64_t key, const int64_t &count) {
    if (count == 0) return;
    PutVar(w, static_cast<uint64_t>(key) >> 32);
    PutVar(w, static_cast<uint64_t>(key) & 0xffffffffu);
    PutVar(w, static_cast<uint64_t>(count));
  });
  return std::move(w.bytes);
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
    return state.groups ? state.groups->Bytes() : 0;
  }
  return 0;
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
                                      std::string_view version, double value,
                                      int8_t sign) {
  if (requires_rebuild) return;
  pending = true;
  NumDelta d;
  d.off = static_cast<uint32_t>(arena.size());
  d.adsh_len = static_cast<uint32_t>(adsh.size());
  d.tag_len = static_cast<uint32_t>(tag.size());
  d.version_len = static_cast<uint32_t>(version.size());
  d.value = value;
  d.sign = sign;
  arena.append(adsh);
  arena.append(tag);
  arena.append(version);
  deltas.push_back(d);
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

SecEdgarQ4State SecEdgarQ4Module::BuildState(core::AuxEngineServices &) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q4 requires a typed initial scan source");
  }
  q4::Flat flat;
  SecEdgarQ4BridgeFilter filter;
  initial_scan->ScanBridge(filter, [&](const SecEdgarQ4BridgeRow &row) {
    flat.AddRow(row);
  });
  return q4::FlatToState(flat, 1);
}

core::InvalidationPlan
SecEdgarQ4Module::InterpretMutation(const core::MutationBatch &) const {
  // All relevant mutations are staged incrementally through the txn buffer
  // (or force a rebuild from there); nothing needs eager invalidation.
  return {};
}

void SecEdgarQ4Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ4TxnBuffer &>(base_buffer);
  const bool is_update = mutation.kind == core::MutationKind::Update;
  if (is_update && mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }

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
    for (const auto &[row, sign] : rows) {
      std::string_view adsh, tag, version, uom;
      double value = 0.0;
      const Cell a = GetString(*row, kQ4NumAdsh, adsh);
      const Cell t = GetString(*row, kQ4NumTag, tag);
      const Cell v = GetString(*row, kQ4NumVersion, version);
      const Cell u = GetString(*row, kQ4NumUom, uom);
      const Cell x = GetDouble(*row, kQ4NumValue, value);
      if (a == Cell::Bad || t == Cell::Bad || v == Cell::Bad || u == Cell::Bad ||
          x == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (a == Cell::Null || t == Cell::Null || v == Cell::Null ||
          u == Cell::Null || x == Cell::Null || uom != "USD") {
        continue; // cannot join / filtered out
      }
      if (!std::isfinite(value)) {
        buffer.MarkRebuild(); // inf/NaN cannot be subtracted back out
        return;
      }
      buffer.AddNumDelta(adsh, tag, version, value, sign);
    }
    return;
  }

  if (mutation.table == config.sub_table) {
    if (is_update && !Intersects(mutation.updated_columns, {kQ4SubAdsh, kQ4SubCik, kQ4SubSic})) {
      return;
    }
    for (const auto &[row, sign] : rows) {
      (void)sign;
      std::string_view adsh;
      const Cell a = GetString(*row, kQ4SubAdsh, adsh);
      if (a == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (a == Cell::Ok) buffer.AddDirtyAdsh(std::string(adsh));
    }
    return;
  }

  if (mutation.table == config.tag_table) {
    if (is_update && !Intersects(mutation.updated_columns,
                                 {kQ4TagTag, kQ4TagVersion, kQ4TagAbstract, kQ4TagTlabel})) {
      return;
    }
    for (const auto &[row, sign] : rows) {
      (void)sign;
      std::string_view tag, version;
      const Cell t = GetString(*row, kQ4TagTag, tag);
      const Cell v = GetString(*row, kQ4TagVersion, version);
      if (t == Cell::Bad || v == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (t == Cell::Ok && v == Cell::Ok) {
        buffer.AddDirtyTagVersion(std::string(tag), std::string(version));
      }
    }
    return;
  }

  if (mutation.table == config.pre_table) {
    if (is_update && !Intersects(mutation.updated_columns,
                                 {kQ4PreAdsh, kQ4PreStmt, kQ4PreTag, kQ4PreVersion})) {
      return;
    }
    for (const auto &[row, sign] : rows) {
      (void)sign;
      std::string_view adsh, stmt, tag, version;
      const Cell a = GetString(*row, kQ4PreAdsh, adsh);
      const Cell s = GetString(*row, kQ4PreStmt, stmt);
      const Cell t = GetString(*row, kQ4PreTag, tag);
      const Cell v = GetString(*row, kQ4PreVersion, version);
      if (a == Cell::Bad || s == Cell::Bad || t == Cell::Bad || v == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (a != Cell::Ok || t != Cell::Ok || v != Cell::Ok) continue; // cannot join
      if (s == Cell::Null || stmt != "EQ") continue; // not part of the bridge
      buffer.AddDirtyKey(std::string(adsh), std::string(tag), std::string(version));
    }
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
  bool have[4] = {false, false, false, false};
  for (const auto &image : snapshot.partitions) {
    if (const auto *t = AsImage(image, SecEdgarQ4ImageKind::SicCohort);
        t && image.partition == core::SecEdgarQ4Layout::kSicCohort && t->state->cohort) {
      state.cohort = t->state->cohort;
      versions[0] = image.version;
      have[0] = true;
    } else if (const auto *t2 = AsImage(image, SecEdgarQ4ImageKind::PreEqBridge);
               t2 && image.partition == core::SecEdgarQ4Layout::kPreEqBridge &&
               t2->state->bridge) {
      state.bridge = t2->state->bridge;
      versions[1] = image.version;
      have[1] = true;
    } else if (const auto *t3 = AsImage(image, SecEdgarQ4ImageKind::TagTlabel);
               t3 && image.partition == core::SecEdgarQ4Layout::kTagTlabel &&
               t3->state->labels) {
      state.labels = t3->state->labels;
      versions[2] = image.version;
      have[2] = true;
    } else if (const auto *t4 = AsImage(image, SecEdgarQ4ImageKind::GroupAggregates);
               t4 && image.partition == core::SecEdgarQ4Layout::kGroupAggregates &&
               t4->state->groups) {
      state.groups = t4->state->groups;
      versions[3] = image.version;
      have[3] = true;
    }
  }
  return have[0] && have[1] && have[2] && have[3] &&
         state.bridge->generation == state.groups->generation &&
         state.groups->labels == state.labels;
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
  std::unordered_set<std::string> sub_adsh;
  std::unordered_set<std::string> tv_set;
  std::unordered_set<std::string> key_set;
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
                           return a.adsh == b.adsh && a.tag == b.tag &&
                                  a.version == b.version;
                         }),
             dk.end());
    if (!initial_scan || da.size() + dt.size() + dk.size() > kMaxStructuralKeys) {
      return RebuildOutcome(services, "too many structural keys or no scan");
    }
    for (const auto &a : da) sub_adsh.insert(a);
    for (const auto &t : dt) tv_set.insert(Join(t.first, t.second));
    for (const auto &k : dk) key_set.insert(Join(k.adsh, Join(k.tag, k.version)));

    SecEdgarQ4BridgeFilter filter;
    filter.all = false;
    filter.include_num = true;
    filter.adsh = da;
    filter.tag_versions = dt;
    filter.keys = dk;
    std::vector<SecEdgarQ4BridgeRow> rows;
    try {
      initial_scan->ScanBridge(filter, [&](const SecEdgarQ4BridgeRow &row) {
        rows.push_back(row);
      });
    } catch (const std::exception &) {
      return RebuildOutcome(services, "scan failed");
    }

    q4::Flat flat;
    q4::StateToFlat(state, flat);
    // Remove every existing entry the dirty keys can touch.
    std::vector<char> adsh_mark(flat.cohort.adsh.Size(), 0);
    for (const auto &a : da) {
      const uint32_t id = flat.cohort.adsh.Find(a);
      if (id != kQ4None) adsh_mark[id] = 1;
    }
    std::vector<char> tv_mark(flat.tv.Size(), 0);
    for (const auto &t : dt) {
      const uint32_t id = flat.tv.Find(Join(t.first, t.second));
      if (id != kQ4None) tv_mark[id] = 1;
    }
    std::vector<uint32_t> victims;
    for (uint32_t i = 0; i < flat.entries.size(); ++i) {
      const auto &e = flat.entries[i];
      if (e.mult != 0 && (adsh_mark[e.adsh_id] || tv_mark[e.tv_id])) victims.push_back(i);
    }
    for (const auto &k : dk) {
      const uint32_t a = flat.cohort.adsh.Find(k.adsh);
      const uint32_t t = flat.tv.Find(Join(k.tag, k.version));
      if (a == kQ4None || t == kQ4None) continue;
      if (const uint32_t *idx = flat.entry_index.Find(
              static_cast<int64_t>((static_cast<uint64_t>(a) << 32) | t))) {
        victims.push_back(*idx);
      }
    }
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    for (const auto idx : victims) flat.RemoveEntry(idx);
    for (const auto &row : rows) flat.AddRow(row);
    if (flat.bad) {
      return RebuildOutcome(services, "remove/add invariant broke (flat.bad)");
    }
    state = q4::FlatToState(flat, state.bridge->generation + 1);
  }

  if (!buffer.Deltas().empty()) {
    q4::CowApplier applier(state);
    // Inserts before deletes: the final counts are non-negative, so applying
    // all +1 deltas first keeps every intermediate count non-negative even when
    // one batch removes and re-adds the same logical row.
    for (const int8_t pass_sign : {int8_t(1), int8_t(-1)}) {
      for (const auto &d : buffer.Deltas()) {
        if (d.sign != pass_sign) continue;
        const auto adsh = buffer.Adsh(d);
        const auto tag = buffer.Tag(d);
        const auto version = buffer.Version(d);
        if (structural) {
          // The re-read of a dirty key already reflects this transaction's num
          // rows; applying the delta on top would double count them.
          const std::string adsh_s(adsh);
          if (sub_adsh.count(adsh_s) || tv_set.count(Join(tag, version)) ||
              key_set.count(Join(adsh, Join(tag, version)))) {
            continue;
          }
        }
        if (!applier.Apply(adsh, tag, version, d.value, d.sign)) {
          return RebuildOutcome(services, "delta inconsistent with state");
        }
      }
    }
    state = applier.Finish();
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  const auto id = Id();
  if (structural) {
    outcome.updates = MakeImages(state, versions);
  } else {
    outcome.updates.push_back(core::StateImage::Make(
        id, core::SecEdgarQ4Layout::kPreEqBridge, versions[1],
        MakeImage(SecEdgarQ4ImageKind::PreEqBridge, state)));
    outcome.updates.push_back(core::StateImage::Make(
        id, core::SecEdgarQ4Layout::kGroupAggregates, versions[3],
        MakeImage(SecEdgarQ4ImageKind::GroupAggregates, state)));
  }
  return outcome;
}

bool SecEdgarQ4Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ4Module::EmitGroups(const SecEdgarQ4Groups &groups,
                                  core::AuxRowSink &sink) const {
  std::shared_ptr<const SecEdgarQ4Result> result;
  {
    std::lock_guard<std::mutex> lock(groups.cache_mutex);
    if (!groups.cache) {
      groups.cache = q4::ComputeResult(groups);
    }
    result = groups.cache;
  }
  sink.Begin({{{"sic", core::AuxType::Int64, false},
               {"tlabel", core::AuxType::String, true},
               {"stmt", core::AuxType::String, false},
               {"num_companies", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false},
               {"avg_value", core::AuxType::Double, false}}});
  for (const auto &row : result->aux_rows) {
    sink.WriteRow(row);
  }
  sink.End();
}

void SecEdgarQ4Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ4Layout::kGroupAggregates});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::SecEdgarQ4Layout::kGroupAggregates) continue;
    const auto *typed = AsImage(image, SecEdgarQ4ImageKind::GroupAggregates);
    if (typed && typed->state->groups) {
      EmitGroups(*typed->state->groups, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void SecEdgarQ4Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  EmitGroups(*state.groups, sink);
}

} // namespace accelerator_codegen::aux_state::query
