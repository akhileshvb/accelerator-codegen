#include "sec_edgar/q24/common/module.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace accelerator_codegen::aux_state::query {
namespace {

// Physical num column indexes carried in mutation rows / updated_columns.
constexpr std::size_t kNumAdsh = 0, kNumTag = 1, kNumVersion = 2, kNumDdate = 3,
                      kNumUom = 5, kNumValue = 7;
// Pre mutation projection.
constexpr std::size_t kPreAdsh = 0, kPreTag = 1, kPreVersion = 2;

enum class Parse { Ok, Unsupported };

// Reads a nullable string cell as a view into the row (no copy).
Parse ReadStr(const core::AuxRow &row, std::size_t index, Q24Str &out) {
  if (index >= row.values.size()) return Parse::Unsupported;
  const auto &cell = row.values[index];
  if (cell.is_null) {
    out = Q24Str::Null();
    return Parse::Ok;
  }
  const auto *s = std::get_if<std::string>(&cell.payload);
  if (!s) return Parse::Unsupported;
  out = Q24Str::Of(*s);
  return Parse::Ok;
}

Parse ReadDouble(const core::AuxRow &row, std::size_t index, double &value,
                 bool &is_null) {
  if (index >= row.values.size()) return Parse::Unsupported;
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return Parse::Ok;
  }
  is_null = false;
  if (const auto *d = std::get_if<double>(&cell.payload)) {
    value = *d;
    return Parse::Ok;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    value = static_cast<double>(*i);
    return Parse::Ok;
  }
  if (const auto *dec = std::get_if<core::ScaledDecimal>(&cell.payload)) {
    value = static_cast<double>(dec->value);
    for (int32_t i = 0; i < dec->scale; ++i) value /= 10.0;
    return Parse::Ok;
  }
  return Parse::Unsupported;
}

// ddate is an INTEGER column (YYYYMMDD). Values outside int32 cannot lie in
// the query's range, so they parse as "out of range" rather than failing.
Parse ReadDdate(const core::AuxRow &row, std::size_t index, bool &in_range) {
  if (index >= row.values.size()) return Parse::Unsupported;
  const auto &cell = row.values[index];
  if (cell.is_null) {
    in_range = false;
    return Parse::Ok;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    in_range = *i >= kSecEdgarQ24FirstDate && *i <= kSecEdgarQ24LastDate;
    return Parse::Ok;
  }
  return Parse::Unsupported;
}

struct NumRow {
  Q24Str adsh, tag, version;
  double value = 0.0;
  bool relevant = false;
};

Parse ParseNum(const core::AuxRow &row, NumRow &out) {
  Q24Str uom;
  bool in_range = false;
  bool value_null = true;
  if (ReadStr(row, kNumAdsh, out.adsh) != Parse::Ok ||
      ReadStr(row, kNumTag, out.tag) != Parse::Ok ||
      ReadStr(row, kNumVersion, out.version) != Parse::Ok ||
      ReadStr(row, kNumUom, uom) != Parse::Ok ||
      ReadDdate(row, kNumDdate, in_range) != Parse::Ok ||
      ReadDouble(row, kNumValue, out.value, value_null) != Parse::Ok)
    return Parse::Unsupported;
  out.relevant = in_range && !value_null && !uom.null &&
                 uom.size == std::strlen(kSecEdgarQ24Uom) &&
                 std::memcmp(uom.data, kSecEdgarQ24Uom, uom.size) == 0;
  return Parse::Ok;
}

std::string MakeKey(const Q24Str &a, const Q24Str &t, const Q24Str &v) {
  std::string key;
  key.reserve(15 + a.size + t.size + v.size);
  Q24AppendComponent(key, a);
  Q24AppendComponent(key, t);
  Q24AppendComponent(key, v);
  return key;
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan PartitionSet(bool pre, bool groups) {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  if (pre)
    plan.partitions.insert(
        static_cast<::accelerator_codegen::aux_state::PartitionKey>(
            core::SecEdgarQ24Layout::kPreMembership));
  if (groups)
    plan.partitions.insert(
        static_cast<::accelerator_codegen::aux_state::PartitionKey>(
            core::SecEdgarQ24Layout::kGroupAggregates));
  return plan;
}

std::shared_ptr<const SecEdgarQ24StateImage>
MakeImage(SecEdgarQ24ImageKind kind,
          const std::shared_ptr<const SecEdgarQ24State> &state) {
  auto image = std::make_shared<SecEdgarQ24StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ24State *AsState(const core::StateImage &image,
                                SecEdgarQ24ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ24StateImage>();
  return payload && payload->kind == kind ? payload->state.get() : nullptr;
}

bool NumColumnRelevant(core::ColumnId c) {
  return c == kNumAdsh || c == kNumTag || c == kNumVersion || c == kNumDdate ||
         c == kNumUom || c == kNumValue;
}

bool PreColumnRelevant(core::ColumnId c) {
  return c == kPreAdsh || c == kPreTag || c == kPreVersion;
}

bool UpdateTouches(const core::MutationBatch &m, bool (*relevant)(core::ColumnId)) {
  if (m.updated_columns.empty()) return true; // unknown: be conservative
  for (const auto c : m.updated_columns)
    if (relevant(c)) return true;
  return false;
}

// ── byte helpers for the footprint encoder ─────────────────────────────────
template <typename T> void PutRaw(std::vector<uint8_t> &out, const T *p, std::size_t n) {
  const auto *b = reinterpret_cast<const uint8_t *>(p);
  out.insert(out.end(), b, b + n * sizeof(T));
}

void PutDict(std::vector<uint8_t> &out, const Q24StringDict &d) {
  const uint32_t n = d.size();
  PutRaw(out, &n, 1);
  PutRaw(out, d.arena().data(), d.arena().size());
  PutRaw(out, d.offsets().data(), d.offsets().size());
}

void PutAccMap(std::vector<uint8_t> &out, const Q24AccMap &m) {
  m.ForEach([&](const std::string &key, const Q24Acc &acc) {
    const uint32_t n = static_cast<uint32_t>(key.size());
    PutRaw(out, &n, 1);
    PutRaw(out, key.data(), key.size());
    PutRaw(out, &acc.cnt, 1);
    PutRaw(out, &acc.sum, 1);
    PutRaw(out, &acc.comp, 1);
  });
}

} // namespace

// ── encode (footprint accounting) ──────────────────────────────────────────

std::vector<uint8_t> EncodeSecEdgarQ24PreMembership(const SecEdgarQ24State &s) {
  std::vector<uint8_t> out;
  if (!s.pre || !s.pre->base) return out;
  const auto &b = *s.pre->base;
  out.reserve(b.Bytes());
  PutDict(out, b.adsh);
  PutDict(out, b.tag);
  PutDict(out, b.version);
  PutRaw(out, b.pairs.pairs().data(), b.pairs.pairs().size());
  PutRaw(out, b.bits256.data(), b.bits256.size());
  PutRaw(out, b.off16.data(), b.off16.size());
  PutRaw(out, b.off32.data(), b.off32.size());
  PutRaw(out, b.ids16.data(), b.ids16.size());
  PutRaw(out, b.ids32.data(), b.ids32.size());
  if (s.pre->overlay)
    for (const auto &k : s.pre->overlay->keys) PutRaw(out, k.data(), k.size());
  return out;
}

std::vector<uint8_t> EncodeSecEdgarQ24TagVersions(const SecEdgarQ24State &) {
  return {};
}

std::vector<uint8_t> EncodeSecEdgarQ24Aggregates(const SecEdgarQ24State &s) {
  std::vector<uint8_t> out;
  if (!s.groups) return out;
  PutAccMap(out, s.groups->orphans);
  PutAccMap(out, s.groups->groups);
  return out;
}

std::vector<SecEdgarQ24OutputRow>
BuildSecEdgarQ24Output(const SecEdgarQ24State &state) {
  std::vector<SecEdgarQ24OutputRow> rows;
  if (!state.groups) return rows;
  for (const auto &r : state.groups->Result())
    rows.push_back({r.tag, r.version, r.tag_null, r.version_null, r.count,
                    r.total});
  return rows;
}

// ── txn buffer ─────────────────────────────────────────────────────────────

bool SecEdgarQ24TxnBuffer::HasPendingChanges() const {
  return requires_rebuild || !ops.empty();
}

void SecEdgarQ24TxnBuffer::Clear() {
  requires_rebuild = false;
  ops.clear();
}

bool SecEdgarQ24TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ24TxnBuffer::Op> &SecEdgarQ24TxnBuffer::Ops() const {
  return ops;
}

void SecEdgarQ24TxnBuffer::MarkRebuild() {
  requires_rebuild = true;
  ops.clear();
}

void SecEdgarQ24TxnBuffer::AddOp(Op op) {
  if (requires_rebuild) return;
  ops.push_back(std::move(op));
}

// ── module ─────────────────────────────────────────────────────────────────

core::AcceleratorId SecEdgarQ24Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ24Module::Dependencies() const {
  return {{config.num_table,
           {static_cast<core::ColumnId>(kNumAdsh),
            static_cast<core::ColumnId>(kNumTag),
            static_cast<core::ColumnId>(kNumVersion),
            static_cast<core::ColumnId>(kNumDdate),
            static_cast<core::ColumnId>(kNumUom),
            static_cast<core::ColumnId>(kNumValue)},
           config.num_attnums},
          {config.pre_table,
           {static_cast<core::ColumnId>(kPreAdsh),
            static_cast<core::ColumnId>(kPreTag),
            static_cast<core::ColumnId>(kPreVersion)},
           config.pre_attnums}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ24Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ24TxnBuffer>();
}

void SecEdgarQ24Module::EmitStateImages(const char *accelerator,
                                        const SecEdgarQ24State &state,
                                        core::StateBuilder &builder) {
  auto pre_state = std::make_shared<SecEdgarQ24State>();
  pre_state->pre = state.pre;
  auto group_state = std::make_shared<SecEdgarQ24State>();
  group_state->groups = state.groups;
  builder.Write(core::StateImage::Make(
      accelerator, core::SecEdgarQ24Layout::kPreMembership, 0,
      MakeImage(SecEdgarQ24ImageKind::PreMembership, pre_state)));
  builder.Write(core::StateImage::Make(
      accelerator, core::SecEdgarQ24Layout::kGroupAggregates, 0,
      MakeImage(SecEdgarQ24ImageKind::Aggregates, group_state)));
  builder.Finish();
}

void SecEdgarQ24Module::Build(core::AuxEngineServices &services,
                              core::StateBuilder &builder) {
  EmitStateImages(kId, BuildState(services), builder);
}

core::InvalidationPlan SecEdgarQ24Module::InterpretMutation(
    const core::MutationBatch &mutation) const {
  if (mutation.table == config.pre_table) {
    if (mutation.kind == core::MutationKind::Update &&
        !UpdateTouches(mutation, PreColumnRelevant))
      return {};
    // Insert / delete / key update all maintain incrementally (deletes and
    // re-keys resolve the touched triples from the base tables at Stage).
    return PartitionSet(true, true);
  }
  if (mutation.table != config.num_table) return {};
  if (mutation.kind == core::MutationKind::Update &&
      !UpdateTouches(mutation, NumColumnRelevant))
    return {};
  return PartitionSet(false, true);
}

void SecEdgarQ24Module::AccumulateMutation(
    const core::MutationBatch &mutation, core::AuxEngineServices &,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ24TxnBuffer &>(base_buffer);
  if (buffer.RequiresRebuild()) return;

  if (mutation.table == config.pre_table) {
    if (mutation.kind == core::MutationKind::Update &&
        !UpdateTouches(mutation, PreColumnRelevant))
      return;
    if (mutation.kind != core::MutationKind::Insert) {
      // Removing / re-keying a pre triple can turn matched num rows into
      // orphans; the triple is resolved from the base tables at Stage.
      const auto touch = [&](const core::AuxRow &row) {
        Q24Str a, t, v;
        if (ReadStr(row, kPreAdsh, a) != Parse::Ok ||
            ReadStr(row, kPreTag, t) != Parse::Ok ||
            ReadStr(row, kPreVersion, v) != Parse::Ok) {
          buffer.MarkRebuild();
          return;
        }
        if (a.null || t.null || v.null) return; // can never match a num row
        SecEdgarQ24TxnBuffer::Op op;
        op.kind = SecEdgarQ24TxnBuffer::Op::Kind::PreTouch;
        op.key = MakeKey(a, t, v);
        buffer.AddOp(std::move(op));
      };
      for (const auto &row : mutation.old_rows) touch(row);
      if (mutation.kind == core::MutationKind::Update) {
        if (mutation.old_rows.size() != mutation.new_rows.size()) {
          buffer.MarkRebuild();
          return;
        }
        for (const auto &row : mutation.new_rows) touch(row);
      }
      return;
    }
    for (const auto &row : mutation.new_rows) {
      Q24Str a, t, v;
      if (ReadStr(row, kPreAdsh, a) != Parse::Ok ||
          ReadStr(row, kPreTag, t) != Parse::Ok ||
          ReadStr(row, kPreVersion, v) != Parse::Ok) {
        buffer.MarkRebuild();
        return;
      }
      if (a.null || t.null || v.null) continue; // can never match a num row
      SecEdgarQ24TxnBuffer::Op op;
      op.kind = SecEdgarQ24TxnBuffer::Op::Kind::PreInsert;
      op.key = MakeKey(a, t, v);
      buffer.AddOp(std::move(op));
    }
    return;
  }
  if (mutation.table != config.num_table) return;
  if (mutation.kind == core::MutationKind::Update &&
      !UpdateTouches(mutation, NumColumnRelevant))
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    NumRow parsed;
    if (ParseNum(row, parsed) != Parse::Ok ||
        (parsed.relevant && !std::isfinite(parsed.value))) {
      buffer.MarkRebuild();
      return;
    }
    if (!parsed.relevant) return;
    SecEdgarQ24TxnBuffer::Op op;
    op.kind = SecEdgarQ24TxnBuffer::Op::Kind::Num;
    op.sign = sign;
    op.value = parsed.value;
    op.key = MakeKey(parsed.adsh, parsed.tag, parsed.version);
    buffer.AddOp(std::move(op));
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows) add(row, 1);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows) add(row, -1);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
SecEdgarQ24Module::StageMaintenance(core::AuxEngineServices &services,
                                    const core::InvalidationPlan &,
                                    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ24TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const auto rebuild_outcome = [&](bool whole_state) {
    core::StagedMaintenanceOutcome out;
    const auto decision = services.ChooseMaintenance(
        {whole_state ? 0u : static_cast<uint64_t>(buffer.Ops().size()), 0, false,
         false, true, true, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    out.action = decision.action == core::MaintenanceAction::ApplyDelta ||
                         decision.action == core::MaintenanceAction::NoOp
                     ? core::MaintenanceAction::RebuildAll
                     : decision.action;
    out.invalidations = AllPartitions();
    out.pending_rebuild = out.invalidations;
    return out;
  };

  if (buffer.RequiresRebuild()) return rebuild_outcome(true);
  const auto &ops = buffer.Ops();
  if (ops.empty()) return outcome;

  bool has_pre = false;
  for (const auto &op : ops)
    if (op.kind == SecEdgarQ24TxnBuffer::Op::Kind::PreInsert) has_pre = true;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(ops.size()), has_pre ? 2u : 1u, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ24Layout::kPreMembership,
       core::SecEdgarQ24Layout::kGroupAggregates});
  std::shared_ptr<const Q24PreState> pre;
  std::shared_ptr<const Q24GroupState> groups;
  core::Version pre_version = 0, groups_version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ24Layout::kPreMembership) {
      if (const auto *s = AsState(image, SecEdgarQ24ImageKind::PreMembership)) {
        pre = s->pre;
        pre_version = image.version;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kGroupAggregates) {
      if (const auto *s = AsState(image, SecEdgarQ24ImageKind::Aggregates)) {
        groups = s->groups;
        groups_version = image.version;
      }
    }
  }
  if (!pre || !pre->base || !groups) {
    core::StagedMaintenanceOutcome out;
    out.action = core::MaintenanceAction::RebuildAll;
    out.invalidations = AllPartitions();
    out.pending_rebuild = out.invalidations;
    return out;
  }

  // Triples whose pre rows were deleted / re-keyed are resolved from the base
  // tables (post-transaction truth); every other op on them is subsumed.
  std::unordered_set<std::string> touched;
  for (const auto &op : ops)
    if (op.kind == SecEdgarQ24TxnBuffer::Op::Kind::PreTouch) touched.insert(op.key);
  std::vector<std::string> touched_keys;
  std::vector<SecEdgarQ24TripleState> touched_states;
  if (!touched.empty()) {
    if (touched.size() > kMaxTouchedTriples || !initial_scan)
      return rebuild_outcome(true);
    std::vector<SecEdgarQ24TripleKey> lookups;
    for (const auto &key : touched) {
      std::size_t off = 0;
      const Q24Str a = Q24ReadComponent(key, off);
      const Q24Str t = Q24ReadComponent(key, off);
      const Q24Str v = Q24ReadComponent(key, off);
      touched_keys.push_back(key);
      lookups.push_back({std::string(a.View()), std::string(t.View()),
                         std::string(v.View())});
    }
    if (!initial_scan->LoadTripleStates(lookups, touched_states) ||
        touched_states.size() != lookups.size())
      return rebuild_outcome(true);
    for (const auto &s : touched_states)
      if (!std::isfinite(s.num_sum) || s.pre_rows < 0 || s.num_rows < 0)
        return rebuild_outcome(true);
  }

  // Replay the transaction's ops in order against copy-on-write views.
  Q24AccMapTxn orphans(groups->orphans);
  Q24AccMapTxn group_map(groups->groups);
  std::shared_ptr<Q24PreOverlay> new_overlay;
  const auto fallback = [&] { return rebuild_outcome(true); };

  const auto in_pre = [&](const Q24Str &a, const Q24Str &t, const Q24Str &v,
                          const std::string &key) {
    const Q24PreOverlay *ov = new_overlay ? new_overlay.get() : pre->overlay.get();
    if (ov && ov->removed.count(key) != 0) return false; // tombstone
    if (pre->base->Contains(a, t, v)) return true;
    return ov && ov->keys.count(key) != 0;
  };
  const auto mutable_overlay = [&]() -> Q24PreOverlay & {
    if (!new_overlay) {
      new_overlay = std::make_shared<Q24PreOverlay>();
      if (pre->overlay) *new_overlay = *pre->overlay;
    }
    return *new_overlay;
  };
  // Removes the orphan accumulators of `key` (now matched by a pre row).
  // False: inconsistent state (caller rebuilds).
  const auto retract_orphan = [&](const std::string &key,
                                  std::size_t group_off) {
    const Q24Acc *orphan = orphans.Find(key);
    if (!orphan) return true;
    const Q24Acc removed = *orphan;
    const std::string gkey = key.substr(group_off);
    Q24Acc *g = group_map.Find(gkey);
    if (!g || g->cnt < removed.cnt) return false;
    g->cnt -= removed.cnt;
    g->Add(-removed.sum);
    g->Add(-removed.comp);
    const bool empty = g->cnt == 0;
    orphans.Erase(key);
    if (empty) group_map.Erase(gkey);
    return true;
  };

  for (const auto &op : ops) {
    if (!touched.empty() && touched.count(op.key) != 0) continue; // resolved below
    std::size_t off = 0;
    const Q24Str a = Q24ReadComponent(op.key, off);
    const std::size_t group_off = off;
    const Q24Str t = Q24ReadComponent(op.key, off);
    const Q24Str v = Q24ReadComponent(op.key, off);
    if (op.kind == SecEdgarQ24TxnBuffer::Op::Kind::PreInsert) {
      if (in_pre(a, t, v, op.key)) continue;
      auto &ov = mutable_overlay();
      if (pre->base->Contains(a, t, v)) ov.removed.erase(op.key);
      else ov.keys.insert(op.key);
      if (ov.keys.size() + ov.removed.size() > kMaxOverlayKeys) return fallback();
      // Orphans of this triple now match a pre row: retract them.
      if (!retract_orphan(op.key, group_off)) return fallback();
      continue;
    }
    // Num row contribution.
    if (in_pre(a, t, v, op.key)) continue; // matched: not in the anti-join
    const std::string gkey = op.key.substr(group_off);
    if (op.sign > 0) {
      auto &o = orphans.GetOrCreate(op.key);
      o.cnt++;
      o.Add(op.value);
      auto &g = group_map.GetOrCreate(gkey);
      g.cnt++;
      g.Add(op.value);
      if (!std::isfinite(g.Total())) return fallback();
    } else {
      Q24Acc *o = orphans.Find(op.key);
      Q24Acc *g = group_map.Find(gkey);
      if (!o || !g || o->cnt <= 0 || g->cnt <= 0) return fallback();
      o->cnt--;
      o->Add(-op.value);
      g->cnt--;
      g->Add(-op.value);
      if (o->cnt == 0) orphans.Erase(op.key);
      if (g->cnt == 0) group_map.Erase(gkey);
    }
  }

  // Triples resolved from the tables: membership and orphans are set to the
  // post-transaction truth.
  for (std::size_t i = 0; i < touched_keys.size(); ++i) {
    const std::string &key = touched_keys[i];
    const auto &st = touched_states[i];
    std::size_t off = 0;
    const Q24Str a = Q24ReadComponent(key, off);
    const std::size_t group_off = off;
    const Q24Str t = Q24ReadComponent(key, off);
    const Q24Str v = Q24ReadComponent(key, off);
    const bool present = st.pre_rows > 0;
    if (present != in_pre(a, t, v, key)) {
      auto &ov = mutable_overlay();
      const bool in_base = pre->base->Contains(a, t, v);
      if (present) {
        if (in_base) ov.removed.erase(key);
        else ov.keys.insert(key);
      } else {
        if (in_base) ov.removed.insert(key);
        else ov.keys.erase(key);
      }
      if (ov.keys.size() + ov.removed.size() > kMaxOverlayKeys) return fallback();
    }
    if (!retract_orphan(key, group_off)) return fallback();
    if (!present && st.num_rows > 0) {
      auto &o = orphans.GetOrCreate(key);
      o.cnt = st.num_rows;
      o.Add(st.num_sum);
      auto &g = group_map.GetOrCreate(key.substr(group_off));
      g.cnt += st.num_rows;
      g.Add(st.num_sum);
      if (!std::isfinite(g.Total())) return fallback();
    }
  }

  const bool groups_changed = orphans.dirty() || group_map.dirty();
  if (!groups_changed && !new_overlay) return outcome; // all ops were no-ops

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  if (groups_changed) {
    auto next = std::make_shared<Q24GroupState>();
    next->orphans = orphans.Finish();
    next->groups = group_map.Finish();
    auto st = std::make_shared<SecEdgarQ24State>();
    st->groups = std::move(next);
    outcome.updates.push_back(core::StateImage::Make(
        kId, core::SecEdgarQ24Layout::kGroupAggregates, groups_version,
        MakeImage(SecEdgarQ24ImageKind::Aggregates, st)));
  }
  if (new_overlay) {
    auto next = std::make_shared<Q24PreState>();
    next->base = pre->base;
    next->overlay = std::move(new_overlay);
    auto st = std::make_shared<SecEdgarQ24State>();
    st->pre = std::move(next);
    outcome.updates.push_back(core::StateImage::Make(
        kId, core::SecEdgarQ24Layout::kPreMembership, pre_version,
        MakeImage(SecEdgarQ24ImageKind::PreMembership, st)));
  }
  return outcome;
}

bool SecEdgarQ24Module::CheckValidity(core::AuxEngineServices &services,
                                      const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

bool SecEdgarQ24Module::LoadGroups(
    core::AuxEngineServices &services,
    std::shared_ptr<const Q24GroupState> &groups) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ24Layout::kGroupAggregates});
  if (!snapshot.available) return false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::SecEdgarQ24Layout::kGroupAggregates) continue;
    if (const auto *s = AsState(image, SecEdgarQ24ImageKind::Aggregates)) {
      if (s->groups) {
        groups = s->groups;
        return true;
      }
    }
  }
  return false;
}

void SecEdgarQ24Module::Execute(core::AuxEngineServices &services,
                                const core::QueryArguments &arguments,
                                core::AuxRowSink &sink) {
  std::shared_ptr<const Q24GroupState> groups;
  if (!LoadGroups(services, groups)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(*groups, sink);
}

void SecEdgarQ24Module::ExecuteFallback(core::AuxEngineServices &services,
                                        const core::QueryArguments &,
                                        core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  EmitOutput(*state.groups, sink);
}

SecEdgarQ24State
SecEdgarQ24Module::BuildState(core::AuxEngineServices &) const {
  if (!initial_scan)
    throw std::logic_error("SEC EDGAR Q24 requires a typed initial scan source");
  SecEdgarQ24State state;
  const bool trace = std::getenv("ACCEL_Q24_TRACE") != nullptr;
  auto t_start = std::chrono::steady_clock::now();
  auto lap = [&](const char *what) {
    if (!trace) return;
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[q24 build] %-28s %8.1f ms\n", what,
                 std::chrono::duration<double, std::milli>(now - t_start).count());
    t_start = now;
  };

  // 1. pre -> immutable exact membership base.
  std::vector<Q24RawPre> parts(1);
  Q24RawPre &raw = parts[0];
  const bool fast_pre =
      initial_scan->ScanPreBatches([&](const SecEdgarQ24PreBatch &b) {
        raw.AddBatch(b.adsh, b.tag, b.version, b.count);
      });
  if (!fast_pre) {
    initial_scan->ScanPre([&](const SecEdgarQ24PreInput &in) {
      raw.Add(Q24Str::Of(in.adsh), Q24Str::Of(in.tag), Q24Str::Of(in.version));
    });
  }
  lap("pre scan + dictionary ingest");
  auto pre = std::make_shared<Q24PreState>();
  pre->base = BuildQ24PreBase(parts);
  const Q24PreBase &base = *pre->base;
  lap("pre base (pairs, CSR)");

  // 2. qualifying num rows with no pre match are the only rows that matter.
  Q24OrphanAccumulator orphans;
  std::vector<uint8_t> hits;
  double probe_ms = 0;
  const bool fast_num = initial_scan->ScanQualifyingNumBatches(
      [&](const SecEdgarQ24NumBatch &b) {
        const auto probe_start = std::chrono::steady_clock::now();
        struct Timer {
          bool on; double &acc; std::chrono::steady_clock::time_point t0;
          ~Timer() { if (on) acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
        } timer{trace, probe_ms, probe_start};
        hits.resize(b.count);
        base.ContainsBatch(b.adsh, b.tag, b.version, b.count, hits.data());
        for (std::size_t i = 0; i < b.count; ++i)
          if (!hits[i]) orphans.Add(b.adsh[i], b.tag[i], b.version[i], b.value[i]);
      });
  if (!fast_num) {
    initial_scan->ScanNum([&](const SecEdgarQ24NumInput &in) {
      if (in.uom != kSecEdgarQ24Uom || in.value_is_null ||
          in.ddate < kSecEdgarQ24FirstDate || in.ddate > kSecEdgarQ24LastDate)
        return;
      const auto a = Q24Str::Of(in.adsh), t = Q24Str::Of(in.tag),
                 v = Q24Str::Of(in.version);
      if (!base.Contains(a, t, v)) orphans.Add(a, t, v, in.value);
    });
  }
  if (trace) std::fprintf(stderr, "[q24 build]   of which probe callback  %8.1f ms\n", probe_ms);
  lap("num scan + probe");
  state.pre = std::move(pre);
  state.groups = orphans.Finish();
  lap("group state");
  if (trace) {
    const auto &b = *state.pre->base;
    std::fprintf(stderr,
                 "[q24 state] resident bytes: adsh_dict=%zu tag_dict=%zu version_dict=%zu "
                 "pairs=%zu bits256=%zu ids16=%zu ids32=%zu offsets=%zu groups=%zu total=%zu "
                 "(triples=%llu, adsh=%u tags=%u versions=%u pairs=%zu orphans=%zu groups=%zu)\n",
                 b.adsh.Bytes(), b.tag.Bytes(), b.version.Bytes(), b.pairs.Bytes(),
                 b.bits256.capacity() * 8, b.ids16.capacity() * 2, b.ids32.capacity() * 4,
                 (b.off16.capacity() + b.off32.capacity()) * 8,
                 state.groups->Bytes(), state.pre->Bytes() + state.groups->Bytes(),
                 static_cast<unsigned long long>(b.triple_count), b.adsh.size(),
                 b.tag.size(), b.version.size(), b.pairs.size(),
                 state.groups->orphans.size(), state.groups->groups.size());
  }
  return state;
}

void SecEdgarQ24Module::EmitOutput(const Q24GroupState &groups,
                                   core::AuxRowSink &sink) {
  sink.Begin({{{"tag", core::AuxType::String, false},
               {"version", core::AuxType::String, false},
               {"cnt", core::AuxType::Int64, false},
               {"total", core::AuxType::Double, false}}});
  // The top-k rows are derived lazily (once per published state) from the
  // maintained group accumulators and are not persisted separately.
  for (const auto &row : groups.Result()) {
    sink.WriteRow(
        {{row.tag_null ? core::AuxValue::Null(core::AuxType::String)
                       : core::AuxValue::Of(core::AuxType::String, row.tag),
          row.version_null
              ? core::AuxValue::Null(core::AuxType::String)
              : core::AuxValue::Of(core::AuxType::String, row.version),
          core::AuxValue::Of(core::AuxType::Int64, row.count),
          core::AuxValue::Of(core::AuxType::Double, row.total)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
