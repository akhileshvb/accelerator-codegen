#include "sec_edgar/q24/common/module.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
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

// ddate is an INTEGER column (YYYYMMDD). NULL and values that fall in no year
// bucket can never satisfy any parameterised range, so they parse as
// "not bucketable" rather than failing.
Parse ReadDdate(const core::AuxRow &row, std::size_t index, bool &bucketable,
                int32_t &year) {
  if (index >= row.values.size()) return Parse::Unsupported;
  const auto &cell = row.values[index];
  if (cell.is_null) {
    bucketable = false;
    return Parse::Ok;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    bucketable = Q24DdateYear(*i, year);
    return Parse::Ok;
  }
  return Parse::Unsupported;
}

struct NumRow {
  Q24Str adsh, tag, version, uom;
  double value = 0.0;
  int32_t year = 0;
  bool relevant = false;
};

Parse ParseNum(const core::AuxRow &row, NumRow &out) {
  bool bucketable = false;
  bool value_null = true;
  if (ReadStr(row, kNumAdsh, out.adsh) != Parse::Ok ||
      ReadStr(row, kNumTag, out.tag) != Parse::Ok ||
      ReadStr(row, kNumVersion, out.version) != Parse::Ok ||
      ReadStr(row, kNumUom, out.uom) != Parse::Ok ||
      ReadDdate(row, kNumDdate, bucketable, out.year) != Parse::Ok ||
      ReadDouble(row, kNumValue, out.value, value_null) != Parse::Ok)
    return Parse::Unsupported;
  out.relevant = bucketable && !value_null && !out.uom.null;
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

void PutOrphanMap(std::vector<uint8_t> &out, const Q24OrphanMap &m) {
  m.ForEach([&](const std::string &key, const Q24OrphanEntry &entry) {
    const uint32_t n = static_cast<uint32_t>(key.size());
    PutRaw(out, &n, 1);
    PutRaw(out, key.data(), key.size());
    for (const auto &b : entry) {
      PutRaw(out, &b.year, 1);
      PutRaw(out, b.uom.data(), b.uom.size());
      PutRaw(out, &b.acc.cnt, 1);
      PutRaw(out, &b.acc.sum, 1);
      PutRaw(out, &b.acc.comp, 1);
    }
  });
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
  PutOrphanMap(out, s.groups->orphans);
  PutAccMap(out, s.groups->groups);
  return out;
}

std::vector<SecEdgarQ24OutputRow>
BuildSecEdgarQ24Output(const SecEdgarQ24State &state) {
  std::vector<SecEdgarQ24OutputRow> rows;
  if (!state.groups) return rows;
  // The default parameter values' bucket.
  const std::string bucket = Q24BucketKey(
      static_cast<int32_t>(kSecEdgarQ24DefaultYear),
      Q24Str::Of(kSecEdgarQ24DefaultUom, std::strlen(kSecEdgarQ24DefaultUom)));
  for (const auto &r : *state.groups->Result(bucket))
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
    const core::MutationBatch &mutation, core::AuxEngineServices &services,
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

  // Rows whose (adsh, tag, version) has a pre match in the published state
  // are not in the anti-join before or after this write: they need no op.
  // Tested for the whole batch at once (pipelined probes); only the rest are
  // keyed and buffered.  Safe within the transaction: a later pre delete /
  // re-key of the triple resolves it from the tables at Stage (its ops are
  // subsumed), and a later pre insert cannot unmatch it.  An earlier pre
  // insert in this transaction is not in the published state, so the row is
  // conservatively kept.
  const std::size_t total_rows = mutation.old_rows.size() + mutation.new_rows.size();
  thread_local std::vector<NumRow> parsed_rows;
  thread_local std::vector<Q24Str> pa, pt, pv;
  thread_local std::vector<uint8_t> matched;
  parsed_rows.resize(total_rows);
  matched.assign(total_rows, 0);
  {
    std::size_t k = 0;
    for (const auto *rows : {&mutation.old_rows, &mutation.new_rows}) {
      for (const auto &row : *rows) {
        NumRow &parsed = parsed_rows[k++];
        if (ParseNum(row, parsed) != Parse::Ok ||
            (parsed.relevant && !std::isfinite(parsed.value))) {
          buffer.MarkRebuild();
          return;
        }
      }
    }
    std::shared_ptr<const Q24PreState> pre;
    if (total_rows >= kMatchFilterMinRows) {
      const auto snapshot = services.StateStore().ReadPartitions(
          services.CurrentVisibility(), {core::SecEdgarQ24Layout::kPreMembership});
      for (const auto &image : snapshot.partitions)
        if (image.partition == core::SecEdgarQ24Layout::kPreMembership)
          if (const auto *s = AsState(image, SecEdgarQ24ImageKind::PreMembership)) pre = s->pre;
    }
    const bool overlay_empty =
        pre && pre->base &&
        (!pre->overlay || (pre->overlay->keys.empty() && pre->overlay->removed.empty()));
    if (overlay_empty) {
      // An update that keeps the key (e.g. SET value = ...) probes it once.
      const std::size_t old_n = mutation.old_rows.size();
      const bool paired = mutation.kind == core::MutationKind::Update &&
                          old_n == mutation.new_rows.size();
      const auto same = [](const Q24Str &x, const Q24Str &y) {
        return x.null == y.null && x.size == y.size &&
               (x.size == 0 || std::memcmp(x.data, y.data, x.size) == 0);
      };
      thread_local std::vector<uint32_t> probe_of;
      thread_local std::vector<uint8_t> probe_hit;
      probe_of.resize(total_rows);
      pa.clear();
      pt.clear();
      pv.clear();
      for (std::size_t i = 0; i < total_rows; ++i) {
        const NumRow &r = parsed_rows[i];
        if (paired && i >= old_n) {
          const NumRow &o = parsed_rows[i - old_n];
          if (same(o.adsh, r.adsh) && same(o.tag, r.tag) && same(o.version, r.version)) {
            probe_of[i] = probe_of[i - old_n];
            continue;
          }
        }
        probe_of[i] = static_cast<uint32_t>(pa.size());
        pa.push_back(r.adsh);
        pt.push_back(r.tag);
        pv.push_back(r.version);
      }
      probe_hit.resize(pa.size());
      pre->base->ContainsBatch(pa.data(), pt.data(), pv.data(), pa.size(), probe_hit.data());
      for (std::size_t i = 0; i < total_rows; ++i) matched[i] = probe_hit[probe_of[i]];
    }
  }
  // parsed_rows / matched: old rows first, then new rows.
  const std::size_t new_base = mutation.old_rows.size();
  auto add = [&](std::size_t index, int8_t sign) {
    const NumRow &parsed = parsed_rows[index];
    if (!parsed.relevant || matched[index]) return;
    SecEdgarQ24TxnBuffer::Op op;
    op.kind = SecEdgarQ24TxnBuffer::Op::Kind::Num;
    op.sign = sign;
    op.year = parsed.year;
    op.uom.assign(parsed.uom.data, parsed.uom.size);
    op.value = parsed.value;
    op.key = MakeKey(parsed.adsh, parsed.tag, parsed.version);
    buffer.AddOp(std::move(op));
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) add(new_base + i, 1);
    break;
  case core::MutationKind::Delete:
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) add(i, -1);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add(i, -1);
      add(new_base + i, 1);
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
    for (const auto &s : touched_states) {
      if (s.pre_rows < 0) return rebuild_outcome(true);
      for (const auto &b : s.buckets)
        if (!std::isfinite(b.num_sum) || b.num_rows < 0)
          return rebuild_outcome(true);
    }
  }

  // Replay the transaction's ops in order against copy-on-write views.
  Q24OrphanMapTxn orphans(groups->orphans);
  Q24AccMapTxn group_map(groups->groups);
  std::shared_ptr<Q24PreOverlay> new_overlay;
  // Group keys / bucket keys whose accumulators changed.
  std::unordered_set<std::string> touched_groups;
  std::unordered_set<std::string> touched_buckets;
  const auto fallback = [&] { return rebuild_outcome(true); };
  const auto bucket_of = [](int32_t year, const std::string &uom) {
    return Q24BucketKey(year, Q24Str::Of(uom));
  };
  const auto note = [&](const std::string &bkey, const std::string &gkey) {
    touched_groups.insert(gkey);
    touched_buckets.insert(bkey);
  };

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
  // Removes the orphan accumulators of `key` (now matched by a pre row), in
  // every (year, uom) bucket. False: inconsistent state (caller rebuilds).
  const auto retract_orphan = [&](const std::string &key,
                                  std::size_t group_off) {
    const Q24OrphanEntry *entry = orphans.Find(key);
    if (!entry) return true;
    const Q24OrphanEntry removed_entry = *entry; // Erase invalidates it
    for (const auto &removed : removed_entry) {
      const std::string bkey = bucket_of(removed.year, removed.uom);
      const std::string gkey = bkey + key.substr(group_off);
      Q24Acc *g = group_map.FindMut(gkey);
      if (!g || g->cnt < removed.acc.cnt) return false;
      g->cnt -= removed.acc.cnt;
      g->Add(-removed.acc.sum);
      g->Add(-removed.acc.comp);
      if (g->cnt == 0) group_map.Erase(gkey);
      note(bkey, gkey);
    }
    orphans.Erase(key);
    return true;
  };
  const auto find_bucket = [](Q24OrphanEntry &entry, int32_t year,
                              const std::string &uom) -> Q24OrphanBucket * {
    for (auto &b : entry)
      if (b.year == year && b.uom == uom) return &b;
    return nullptr;
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
    const std::string bkey = bucket_of(op.year, op.uom);
    const std::string gkey = bkey + op.key.substr(group_off);
    if (op.sign > 0) {
      auto &entry = orphans.GetOrCreate(op.key);
      Q24OrphanBucket *o = find_bucket(entry, op.year, op.uom);
      if (!o) {
        entry.push_back({op.year, op.uom, Q24Acc()});
        o = &entry.back();
      }
      o->acc.cnt++;
      o->acc.Add(op.value);
      auto &g = group_map.GetOrCreate(gkey);
      g.cnt++;
      g.Add(op.value);
      if (!std::isfinite(g.Total())) return fallback();
    } else {
      const Q24OrphanEntry *probe = orphans.Find(op.key);
      if (!probe) return fallback();
      Q24OrphanEntry *entry = orphans.FindMut(op.key);
      Q24OrphanBucket *o = find_bucket(*entry, op.year, op.uom);
      Q24Acc *g = group_map.FindMut(gkey);
      if (!o || !g || o->acc.cnt <= 0 || g->cnt <= 0) return fallback();
      o->acc.cnt--;
      o->acc.Add(-op.value);
      g->cnt--;
      g->Add(-op.value);
      if (o->acc.cnt == 0) {
        entry->erase(entry->begin() + (o - entry->data()));
        if (entry->empty()) orphans.Erase(op.key);
      }
      if (g->cnt == 0) group_map.Erase(gkey);
    }
    note(bkey, gkey);
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
    if (!present) {
      for (const auto &tb : st.buckets) {
        if (tb.num_rows <= 0) continue;
        const std::string bkey = bucket_of(tb.year, tb.uom);
        const std::string gkey = bkey + key.substr(group_off);
        auto &entry = orphans.GetOrCreate(key);
        Q24OrphanBucket *o = find_bucket(entry, tb.year, tb.uom);
        if (!o) {
          entry.push_back({tb.year, tb.uom, Q24Acc()});
          o = &entry.back();
        }
        o->acc.cnt += tb.num_rows;
        o->acc.Add(tb.num_sum);
        auto &g = group_map.GetOrCreate(gkey);
        g.cnt += tb.num_rows;
        g.Add(tb.num_sum);
        if (!std::isfinite(g.Total())) return fallback();
        note(bkey, gkey);
      }
    }
  }

  // Heavy sets (groups passing HAVING) of the touched buckets.
  Q24HeavyMap heavy = groups->heavy;
  std::unordered_set<std::string> heavy_copied;
  for (const auto &gkey : touched_groups) {
    const std::size_t bsz = 4 + Q24ComponentSize(gkey, 4);
    const std::string bkey = gkey.substr(0, bsz);
    const Q24Acc *g = group_map.Find(gkey);
    const bool want = g && g->cnt >= kSecEdgarQ24MinimumCount;
    const auto it = heavy.find(bkey);
    const bool have = it != heavy.end() && it->second && it->second->count(gkey) != 0;
    if (want == have) continue;
    if (heavy_copied.insert(bkey).second) {
      auto copy = it != heavy.end() && it->second ? std::make_shared<Q24HeavySet>(*it->second)
                                                  : std::make_shared<Q24HeavySet>();
      heavy[bkey] = copy;
    }
    auto &set = const_cast<Q24HeavySet &>(*heavy[bkey]);
    if (want) set.insert(gkey);
    else set.erase(gkey);
  }
  for (const auto &bkey : heavy_copied) {
    const auto it = heavy.find(bkey);
    if (it != heavy.end() && it->second->empty()) heavy.erase(it);
  }

  const bool groups_changed = orphans.dirty() || group_map.dirty();
  if (!groups_changed && !new_overlay) return outcome; // all ops were no-ops

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  if (groups_changed) {
    auto next = std::make_shared<Q24GroupState>();
    next->orphans = orphans.Finish();
    next->groups = group_map.Finish();
    next->heavy = std::move(heavy);
    next->InheritResults(*groups, touched_buckets);
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

bool SecEdgarQ24Module::ParseArguments(const core::QueryArguments &args,
                                       std::string &bucket_key) {
  int64_t year = kSecEdgarQ24DefaultYear;
  Q24Str uom = Q24Str::Of(kSecEdgarQ24DefaultUom, std::strlen(kSecEdgarQ24DefaultUom));
  std::string uom_storage;
  if (!args.values.empty()) {
    if (args.values.size() != 2)
      throw std::invalid_argument(
          "sec_edgar q24 aux expects () or (year BIGINT, uom VARCHAR)");
    const auto &y = args.values[0];
    const auto &u = args.values[1];
    if (y.is_null || u.is_null) return false; // NULL never compares equal
    if (const auto *i = std::get_if<int64_t>(&y.payload)) {
      year = *i;
    } else if (const auto *ui = std::get_if<uint64_t>(&y.payload)) {
      if (*ui > static_cast<uint64_t>(INT32_MAX)) return false;
      year = static_cast<int64_t>(*ui);
    } else if (const auto *d = std::get_if<double>(&y.payload)) {
      if (!(*d == std::floor(*d)) || !(std::fabs(*d) < 9.0e18)) return false;
      year = static_cast<int64_t>(*d);
    } else if (const auto *s = std::get_if<std::string>(&y.payload)) {
      char *end = nullptr;
      errno = 0;
      const long long v = std::strtoll(s->c_str(), &end, 10);
      if (end == s->c_str() || *end != '\0' || errno == ERANGE) return false;
      year = v;
    } else {
      throw std::invalid_argument("sec_edgar q24 aux: unsupported year argument");
    }
    const auto *us = std::get_if<std::string>(&u.payload);
    if (!us) throw std::invalid_argument("sec_edgar q24 aux: uom must be a string");
    uom_storage = *us;
    uom = Q24Str::Of(uom_storage);
  }
  // ddate is an INTEGER: no row can lie in the window of a year whose
  // bucket does not fit int32 (this also bounds Y*10000+1231 in 64 bits).
  if (year < INT32_MIN || year > INT32_MAX) return false;
  bucket_key = Q24BucketKey(static_cast<int32_t>(year), uom);
  return true;
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
  std::string bucket;
  if (!ParseArguments(arguments, bucket)) {
    EmitOutput(Q24GroupState(), std::string(), sink);
    return;
  }
  std::shared_ptr<const Q24GroupState> groups;
  if (!LoadGroups(services, groups)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(*groups, bucket, sink);
}

void SecEdgarQ24Module::ExecuteFallback(core::AuxEngineServices &services,
                                        const core::QueryArguments &arguments,
                                        core::AuxRowSink &sink) {
  std::string bucket;
  if (!ParseArguments(arguments, bucket)) {
    EmitOutput(Q24GroupState(), std::string(), sink);
    return;
  }
  const auto state = BuildState(services);
  EmitOutput(*state.groups, bucket, sink);
}

SecEdgarQ24State
SecEdgarQ24Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("SEC EDGAR Q24 requires a typed initial scan source");
  SecEdgarQ24State state;

  // 1. pre -> immutable exact membership base.
  std::vector<Q24RawPre> parts(1);
  Q24RawPre &raw = parts[0];
  const bool fast_pre =
      initial_scan->ScanPreIds(raw) ||
      initial_scan->ScanPreBatches([&](const SecEdgarQ24PreBatch &b) {
        raw.AddBatch(b.adsh, b.tag, b.version, b.count);
      });
  if (!fast_pre) {
    initial_scan->ScanPre([&](const SecEdgarQ24PreInput &in) {
      raw.Add(Q24Str::Of(in.adsh), Q24Str::Of(in.tag), Q24Str::Of(in.version));
    });
  }
  auto pre = std::make_shared<Q24PreState>();
  pre->base = BuildQ24PreBase(parts, &services);
  const Q24PreBase &base = *pre->base;

  // 2. qualifying num rows with no pre match are the only rows that matter.
  Q24OrphanAccumulator orphans;
  std::vector<uint8_t> hits;
  const bool orphan_scan = initial_scan->ScanNumOrphans(
      base, [&](const SecEdgarQ24NumBatch &b) {
        for (std::size_t i = 0; i < b.count; ++i) {
          int32_t year;
          if (b.uom[i].null || !Q24DdateYear(b.ddate[i], year)) continue;
          orphans.Add(b.adsh[i], b.tag[i], b.version[i], year, b.uom[i], b.value[i]);
        }
      });
  const bool fast_num = orphan_scan || initial_scan->ScanQualifyingNumBatches(
      [&](const SecEdgarQ24NumBatch &b) {
        hits.resize(b.count);
        base.ContainsBatch(b.adsh, b.tag, b.version, b.count, hits.data());
        for (std::size_t i = 0; i < b.count; ++i) {
          int32_t year;
          if (hits[i] || b.uom[i].null || !Q24DdateYear(b.ddate[i], year)) continue;
          orphans.Add(b.adsh[i], b.tag[i], b.version[i], year, b.uom[i], b.value[i]);
        }
      });
  if (!fast_num) {
    initial_scan->ScanNum([&](const SecEdgarQ24NumInput &in) {
      int32_t year;
      if (in.value_is_null || !Q24DdateYear(in.ddate, year)) return;
      const auto a = Q24Str::Of(in.adsh), t = Q24Str::Of(in.tag),
                 v = Q24Str::Of(in.version);
      if (!base.Contains(a, t, v)) orphans.Add(a, t, v, year, Q24Str::Of(in.uom), in.value);
    });
  }
  state.pre = std::move(pre);
  state.groups = orphans.Finish();
  return state;
}

void SecEdgarQ24Module::EmitOutput(const Q24GroupState &groups,
                                   const std::string &bucket_key,
                                   core::AuxRowSink &sink) {
  sink.Begin({{{"tag", core::AuxType::String, false},
               {"version", core::AuxType::String, false},
               {"cnt", core::AuxType::Int64, false},
               {"total", core::AuxType::Double, false}}});
  // The top-k rows of a bucket are derived lazily (once per published state
  // and bucket) from the maintained group accumulators.
  if (!bucket_key.empty()) {
    const auto rows = groups.Result(bucket_key);
    for (const auto &row : *rows) {
      sink.WriteRow(
          {{row.tag_null ? core::AuxValue::Null(core::AuxType::String)
                         : core::AuxValue::Of(core::AuxType::String, row.tag),
            row.version_null
                ? core::AuxValue::Null(core::AuxType::String)
                : core::AuxValue::Of(core::AuxType::String, row.version),
            core::AuxValue::Of(core::AuxType::Int64, row.count),
            core::AuxValue::Of(core::AuxType::Double, row.total)}});
    }
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
