#include "sec_edgar/q4/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kSicCohortMagic = 0x43345151;        // Q4SC
constexpr uint32_t kPreEqBridgeMagic = 0x50345151;      // Q4PB
constexpr uint32_t kTagTlabelMagic = 0x54345151;        // Q4TT
constexpr uint32_t kGroupAggregateMagic = 0x41345151;   // Q4AG
constexpr int32_t kSicMin = 4000;
constexpr int32_t kSicMax = 4999;
constexpr int32_t kMinDistinctCik = 2;
constexpr std::size_t kOutputLimit = 500;


bool ReadString(const core::AuxRow &row, std::size_t index, std::string &value) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload)) {
    return false;
  }
  value = std::get<std::string>(row.values[index].payload);
  return true;
}

bool ReadDouble(const core::AuxRow &row, std::size_t index, double &value,
                bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return true;
  }
  is_null = false;
  if (std::holds_alternative<double>(cell.payload)) {
    value = std::get<double>(cell.payload);
    return true;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    value = static_cast<double>(std::get<int64_t>(cell.payload));
    return true;
  }
  return false;
}

bool ReadInt32(const core::AuxRow &row, std::size_t index, int32_t &value) {
  if (index >= row.values.size() || row.values[index].is_null) {
    return false;
  }
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    const auto raw = std::get<int64_t>(cell.payload);
    if (raw < std::numeric_limits<int32_t>::min() ||
        raw > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
  }
  return false;
}

bool SicInRange(int32_t sic) { return sic >= kSicMin && sic <= kSicMax; }

uint32_t FindGroup(const std::vector<SecEdgarQ4GroupKey> &groups,
                   const SecEdgarQ4GroupKey &key) {
  const auto it = std::lower_bound(groups.begin(), groups.end(), key);
  if (it == groups.end() || *it != key) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(it - groups.begin());
}

uint32_t InternTlabel(SecEdgarQ4State &state, const std::string &tlabel) {
  const auto it = std::find(state.tlabels.begin(), state.tlabels.end(), tlabel);
  if (it != state.tlabels.end()) {
    return static_cast<uint32_t>(it - state.tlabels.begin());
  }
  const uint32_t id = static_cast<uint32_t>(state.tlabels.size());
  state.tlabels.push_back(tlabel);
  return id;
}

uint32_t InternTagVersion(SecEdgarQ4State &state,
                          const SecEdgarQ4TagVersionKey &key,
                          uint32_t tlabel_id) {
  const auto it = state.tag_version_index.find(key);
  if (it != state.tag_version_index.end()) {
    return it->second;
  }
  const uint32_t id = static_cast<uint32_t>(state.tag_versions.size());
  state.tag_versions.push_back(key);
  state.tag_version_index.emplace(key, id);
  state.tlabel_ids_by_tag_version.push_back(tlabel_id);
  return id;
}

uint32_t InternGroup(SecEdgarQ4State &state, int32_t sic, uint32_t tlabel_id) {
  const SecEdgarQ4GroupKey key{sic, tlabel_id};
  for (uint32_t id = 0; id < state.group_keys.size(); ++id) {
    if (state.group_keys[id] == key) {
      return id;
    }
  }
  const uint32_t id = static_cast<uint32_t>(state.group_keys.size());
  state.group_keys.push_back(key);
  state.aggregates.push_back({});
  if (state.cik_bit_word_count > 0) {
    state.aggregates.back().cik_bits.assign(state.cik_bit_word_count, 0);
  }
  return id;
}

void EnsureCikBitCapacity(SecEdgarQ4GroupAggregate &group,
                            uint32_t cik_bit_word_count) {
  if (group.cik_bits.size() != cik_bit_word_count) {
    group.cik_bits.assign(cik_bit_word_count, 0);
  }
}

void AccumulateGroup(SecEdgarQ4GroupAggregate &group, uint32_t cik_dense_id,
                     uint32_t cik_bit_word_count, uint32_t cik_dense_count,
                     double value, uint32_t multiplicity) {
  EnsureCikBitCapacity(group, cik_bit_word_count);
  const auto delta_sum = value * static_cast<double>(multiplicity);
  group.sum += delta_sum;
  group.count += static_cast<int64_t>(multiplicity);
  if (cik_bit_word_count > 0 && cik_dense_id < cik_dense_count) {
    const auto word = cik_dense_id >> 6;
    const auto bit = cik_dense_id & 63U;
    const auto mask = uint64_t(1) << bit;
    if ((group.cik_bits[word] & mask) == 0) {
      group.cik_bits[word] |= mask;
      group.distinct_cik_count++;
    }
  }
}

void ApplyNumContribution(SecEdgarQ4State &state, const std::string &adsh,
                          const SecEdgarQ4TagVersionKey &tag_version,
                          double value, int8_t sign) {
  const auto sub_it = state.sub_lookup.find(adsh);
  if (sub_it == state.sub_lookup.end()) {
    return;
  }
  const auto tag_it = state.tag_version_index.find(tag_version);
  if (tag_it == state.tag_version_index.end()) {
    return;
  }
  const auto pre_adsh_it = state.pre_by_adsh.find(adsh);
  if (pre_adsh_it == state.pre_by_adsh.end()) {
    return;
  }
  const auto pre_entry_it = pre_adsh_it->second.find(tag_it->second);
  if (pre_entry_it == pre_adsh_it->second.end() ||
      pre_entry_it->second.multiplicity == 0) {
    return;
  }
  const auto &resolved = pre_entry_it->second;
  if (resolved.group_id >= state.aggregates.size()) {
    return;
  }
  const auto scaled_value = static_cast<double>(sign) * value;
  AccumulateGroup(state.aggregates[resolved.group_id], resolved.cik_dense_id,
                  state.cik_bit_word_count, state.cik_dense_count, scaled_value,
                  resolved.multiplicity);
}

void BuildSubLookup(SecEdgarQ4State &state,
                    const SecEdgarQ4InitialScan *initial_scan) {
  initial_scan->ScanSub([&](const SecEdgarQ4SubInput &input) {
    if (!SicInRange(input.sic)) {
      return;
    }
    auto &info = state.sub_lookup[input.adsh];
    info.cik = input.cik;
    info.sic = input.sic;
    if (state.cik_dense_index.find(input.cik) == state.cik_dense_index.end()) {
      state.cik_dense_index[input.cik] = state.cik_dense_count++;
    }
    info.cik_dense_id = state.cik_dense_index[input.cik];
  });
  state.cik_bit_word_count =
      state.cik_dense_count == 0 ? 0 : (state.cik_dense_count + 63U) / 64U;
}

void BuildTagLookup(SecEdgarQ4State &state,
                    const SecEdgarQ4InitialScan *initial_scan) {
  // Only keep tag/tlabel entries that can appear in the SIC+EQ bridge.
  // Storing the full abstract=0 tag table (~1M rows on SF3) dominates aux RSS.
  std::set<SecEdgarQ4TagVersionKey> needed;
  initial_scan->ScanPre([&](const SecEdgarQ4PreInput &input) {
    if (input.stmt != "EQ") {
      return;
    }
    if (state.sub_lookup.find(input.adsh) == state.sub_lookup.end()) {
      return;
    }
    needed.insert({input.tag, input.version});
  });
  initial_scan->ScanTag([&](const SecEdgarQ4TagInput &input) {
    if (input.abstract_flag != 0) {
      return;
    }
    if (needed.find({input.tag, input.version}) == needed.end()) {
      return;
    }
    const uint32_t tlabel_id = InternTlabel(state, input.tlabel);
    InternTagVersion(state, {input.tag, input.version}, tlabel_id);
  });
}

void BuildPreBridge(SecEdgarQ4State &state,
                    const SecEdgarQ4InitialScan *initial_scan) {
  initial_scan->ScanPre([&](const SecEdgarQ4PreInput &input) {
    if (input.stmt != "EQ") {
      return;
    }
    const auto sub_it = state.sub_lookup.find(input.adsh);
    if (sub_it == state.sub_lookup.end()) {
      return;
    }
    const auto tag_it =
        state.tag_version_index.find({input.tag, input.version});
    if (tag_it == state.tag_version_index.end()) {
      return;
    }
    const uint32_t tlabel_id = state.tlabel_ids_by_tag_version[tag_it->second];
    const uint32_t group_id =
        InternGroup(state, sub_it->second.sic, tlabel_id);
    auto &resolved = state.pre_by_adsh[input.adsh][tag_it->second];
    resolved.group_id = group_id;
    resolved.cik_dense_id = sub_it->second.cik_dense_id;
    resolved.multiplicity++;
  });
}

void BuildAggregates(SecEdgarQ4State &state,
                     const SecEdgarQ4InitialScan *initial_scan) {
  initial_scan->ScanNum([&](const SecEdgarQ4NumInput &input) {
    if (input.uom != "USD" || input.value_is_null) {
      return;
    }
    ApplyNumContribution(state, input.adsh, {input.tag, input.version},
                         input.value, 1);
  });
}

std::shared_ptr<const SecEdgarQ4StateImage>
MakeStateImage(SecEdgarQ4ImageKind kind,
               const std::shared_ptr<const SecEdgarQ4State> &state) {
  auto image = std::make_shared<SecEdgarQ4StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ4StateImage *AsQ4Image(const core::StateImage &image,
                                      SecEdgarQ4ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ4StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ4State &state,
                     core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ4State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ4Layout::kAccelerator, core::SecEdgarQ4Layout::kSicCohort,
      0, MakeStateImage(SecEdgarQ4ImageKind::SicCohort, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ4Layout::kAccelerator,
      core::SecEdgarQ4Layout::kPreEqBridge, 0,
      MakeStateImage(SecEdgarQ4ImageKind::PreEqBridge, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ4Layout::kAccelerator, core::SecEdgarQ4Layout::kTagTlabel,
      0, MakeStateImage(SecEdgarQ4ImageKind::TagTlabel, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ4Layout::kAccelerator,
      core::SecEdgarQ4Layout::kGroupAggregates, 0,
      MakeStateImage(SecEdgarQ4ImageKind::GroupAggregates, typed_state)));
  builder.Finish();
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan AggregatePartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          core::SecEdgarQ4Layout::kGroupAggregates));
  return plan;
}

} // namespace

std::size_t SecEdgarQ4TagVersionKeyHash::operator()(
    const SecEdgarQ4TagVersionKey &key) const noexcept {
  return std::hash<std::string>{}(key.tag) ^
         (std::hash<std::string>{}(key.version) << 1);
}

std::size_t SecEdgarQ4GroupKeyHash::operator()(
    const SecEdgarQ4GroupKey &key) const noexcept {
  return (static_cast<std::size_t>(key.sic) << 32) ^ key.tlabel_id;
}

std::vector<uint8_t> EncodeSecEdgarQ4SicCohort(const SecEdgarQ4State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kSicCohortMagic, kFormatVersion, static_cast<uint32_t>(state.sub_lookup.size()));
  writer.U32(state.cik_dense_count);
  writer.U32(state.cik_bit_word_count);
  std::vector<std::pair<std::string, SecEdgarQ4SubInfo>> entries(
      state.sub_lookup.begin(), state.sub_lookup.end());
  std::sort(entries.begin(), entries.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  for (const auto &entry : entries) {
    writer.String(entry.first);
    writer.I32(entry.second.cik);
    writer.I32(entry.second.sic);
    writer.U32(entry.second.cik_dense_id);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ4PreEqBridge(const SecEdgarQ4State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kPreEqBridgeMagic, kFormatVersion, static_cast<uint32_t>(state.pre_by_adsh.size()));
  std::vector<std::pair<std::string,
                        std::unordered_map<uint32_t, SecEdgarQ4ResolvedPreTriple>>>
      entries(state.pre_by_adsh.begin(), state.pre_by_adsh.end());
  std::sort(entries.begin(), entries.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  for (const auto &entry : entries) {
    writer.String(entry.first);
    writer.U32(static_cast<uint32_t>(entry.second.size()));
    std::vector<std::pair<uint32_t, SecEdgarQ4ResolvedPreTriple>> triples(
        entry.second.begin(), entry.second.end());
    std::sort(triples.begin(), triples.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.first < rhs.first;
              });
    for (const auto &triple : triples) {
      writer.U32(triple.first);
      writer.U32(triple.second.group_id);
      writer.U32(triple.second.cik_dense_id);
      writer.U32(triple.second.multiplicity);
    }
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ4TagTlabel(const SecEdgarQ4State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kTagTlabelMagic, kFormatVersion, static_cast<uint32_t>(state.tag_versions.size()));
  writer.U32(static_cast<uint32_t>(state.tlabels.size()));
  for (const auto &tlabel : state.tlabels) {
    writer.String(tlabel);
  }
  for (const auto &key : state.tag_versions) {
    writer.String(key.tag);
    writer.String(key.version);
  }
  for (const auto tlabel_id : state.tlabel_ids_by_tag_version) {
    writer.U32(tlabel_id);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeSecEdgarQ4GroupAggregates(const SecEdgarQ4State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kGroupAggregateMagic, kFormatVersion, static_cast<uint32_t>(state.group_keys.size()));
  for (const auto &key : state.group_keys) {
    writer.I32(key.sic);
    writer.U32(key.tlabel_id);
  }
  for (const auto &aggregate : state.aggregates) {
    writer.I64(aggregate.count);
    writer.Double(aggregate.sum);
    writer.U32(aggregate.distinct_cik_count);
    writer.U32(static_cast<uint32_t>(aggregate.cik_bits.size()));
    for (const auto word : aggregate.cik_bits) {
      writer.U64(word);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ4SicCohort(const std::vector<uint8_t> &bytes,
                                 SecEdgarQ4State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kSicCohortMagic, kFormatVersion, count) ||
      !reader.U32(state.cik_dense_count) ||
      !reader.U32(state.cik_bit_word_count)) {
    return false;
  }
  state.sub_lookup.clear();
  state.cik_dense_index.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string adsh;
    SecEdgarQ4SubInfo info;
    if (!reader.String(adsh) || !reader.I32(info.cik) || !reader.I32(info.sic) ||
        !reader.U32(info.cik_dense_id)) {
      return false;
    }
    state.sub_lookup.emplace(std::move(adsh), info);
    state.cik_dense_index[info.cik] = info.cik_dense_id;
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ4PreEqBridge(const std::vector<uint8_t> &bytes,
                                   SecEdgarQ4State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kPreEqBridgeMagic, kFormatVersion, count)) {
    return false;
  }
  state.pre_by_adsh.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string adsh;
    uint32_t triple_count = 0;
    if (!reader.String(adsh) || !reader.U32(triple_count)) {
      return false;
    }
    auto &entries = state.pre_by_adsh[adsh];
    for (uint32_t j = 0; j < triple_count; ++j) {
      uint32_t tag_version_id = 0;
      SecEdgarQ4ResolvedPreTriple triple;
      if (!reader.U32(tag_version_id) || !reader.U32(triple.group_id) ||
          !reader.U32(triple.cik_dense_id) ||
          !reader.U32(triple.multiplicity)) {
        return false;
      }
      entries.emplace(tag_version_id, triple);
    }
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ4TagTlabel(const std::vector<uint8_t> &bytes,
                                 SecEdgarQ4State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  uint32_t tlabel_count = 0;
  if (!core::ReadImageHeader(reader, kTagTlabelMagic, kFormatVersion, count) || !reader.U32(tlabel_count)) {
    return false;
  }
  state.tlabels.clear();
  state.tlabels.resize(tlabel_count);
  for (uint32_t i = 0; i < tlabel_count; ++i) {
    if (!reader.String(state.tlabels[i])) {
      return false;
    }
  }
  state.tag_versions.clear();
  state.tag_version_index.clear();
  state.tlabel_ids_by_tag_version.clear();
  state.tag_versions.reserve(count);
  state.tlabel_ids_by_tag_version.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ4TagVersionKey key;
    if (!reader.String(key.tag) || !reader.String(key.version)) {
      return false;
    }
    state.tag_version_index.emplace(key,
                                  static_cast<uint32_t>(state.tag_versions.size()));
    state.tag_versions.push_back(std::move(key));
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t tlabel_id = 0;
    if (!reader.U32(tlabel_id)) {
      return false;
    }
    state.tlabel_ids_by_tag_version.push_back(tlabel_id);
  }
  return reader.offset == bytes.size() &&
         state.tag_versions.size() == state.tlabel_ids_by_tag_version.size() &&
         std::is_sorted(state.tag_versions.begin(), state.tag_versions.end());
}

bool DecodeSecEdgarQ4GroupAggregates(const std::vector<uint8_t> &bytes,
                                       SecEdgarQ4State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kGroupAggregateMagic, kFormatVersion, count)) {
    return false;
  }
  state.group_keys.clear();
  state.group_keys.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ4GroupKey key;
    if (!reader.I32(key.sic) || !reader.U32(key.tlabel_id)) {
      return false;
    }
    state.group_keys.push_back(key);
  }
  state.aggregates.clear();
  state.aggregates.resize(count);
  for (auto &aggregate : state.aggregates) {
    uint32_t bit_count = 0;
    if (!reader.I64(aggregate.count) || !reader.Double(aggregate.sum) ||
        !reader.U32(aggregate.distinct_cik_count) || !reader.U32(bit_count)) {
      return false;
    }
    aggregate.cik_bits.resize(bit_count);
    for (uint32_t i = 0; i < bit_count; ++i) {
      if (!reader.U64(aggregate.cik_bits[i])) {
        return false;
      }
    }
  }
  return reader.offset == bytes.size() &&
         std::is_sorted(state.group_keys.begin(), state.group_keys.end());
}

std::vector<SecEdgarQ4OutputRow>
BuildSecEdgarQ4Output(const SecEdgarQ4State &state) {
  std::vector<uint32_t> candidates;
  for (uint32_t id = 0;
       id < state.aggregates.size() && id < state.group_keys.size(); ++id) {
    const auto &aggregate = state.aggregates[id];
    if (aggregate.count > 0 &&
        static_cast<int64_t>(aggregate.distinct_cik_count) >= kMinDistinctCik) {
      candidates.push_back(id);
    }
  }
  const auto compare = [&](uint32_t lhs, uint32_t rhs) {
    const auto &left = state.aggregates[lhs];
    const auto &right = state.aggregates[rhs];
    if (left.sum != right.sum) {
      return left.sum > right.sum;
    }
    const auto &left_key = state.group_keys[lhs];
    const auto &right_key = state.group_keys[rhs];
    if (left_key.sic != right_key.sic) {
      return left_key.sic < right_key.sic;
    }
    const auto left_tlabel = left_key.tlabel_id < state.tlabels.size()
                                 ? state.tlabels[left_key.tlabel_id]
                                 : std::string();
    const auto right_tlabel = right_key.tlabel_id < state.tlabels.size()
                                  ? state.tlabels[right_key.tlabel_id]
                                  : std::string();
    return left_tlabel < right_tlabel;
  };
  if (candidates.size() > kOutputLimit) {
    std::partial_sort(candidates.begin(),
                      candidates.begin() + static_cast<ptrdiff_t>(kOutputLimit),
                      candidates.end(), compare);
    candidates.resize(kOutputLimit);
  } else {
    std::sort(candidates.begin(), candidates.end(), compare);
  }
  std::vector<SecEdgarQ4OutputRow> output;
  output.reserve(candidates.size());
  for (const auto id : candidates) {
    const auto &key = state.group_keys[id];
    const auto &aggregate = state.aggregates[id];
    SecEdgarQ4OutputRow row;
    row.sic = key.sic;
    row.tlabel = key.tlabel_id < state.tlabels.size()
                     ? state.tlabels[key.tlabel_id]
                     : std::string();
    row.stmt = "EQ";
    row.num_companies = static_cast<int64_t>(aggregate.distinct_cik_count);
    row.total_value = aggregate.sum;
    row.avg_value = aggregate.count > 0
                        ? aggregate.sum / static_cast<double>(aggregate.count)
                        : 0.0;
    output.push_back(std::move(row));
  }
  return output;
}

bool SecEdgarQ4TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ4TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ4TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ4TxnBuffer::NumDelta> &
SecEdgarQ4TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ4TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ4TxnBuffer::AddDelta(NumDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ4Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ4Module::Dependencies() const {
  return {{config.num_table, {0, 1, 2, 3, 4}},
          {config.sub_table, {0, 1, 2}},
          {config.tag_table, {0, 1, 2, 3}},
          {config.pre_table, {0, 1, 2, 3}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ4Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ4TxnBuffer>();
}

void SecEdgarQ4Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan
SecEdgarQ4Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.sub_table || mutation.table == config.tag_table ||
      mutation.table == config.pre_table) {
    return AllPartitions();
  }
  if (mutation.table != config.num_table) {
    return {};
  }
  if (mutation.kind == core::MutationKind::Update &&
      !mutation.updated_columns.empty()) {
    bool relevant = false;
    for (const auto column : mutation.updated_columns) {
      // Physical num.value is column 7; key/filter cols are 0..4.
      if (column == 7 || column <= 4) {
        relevant = true;
        break;
      }
    }
    if (!relevant) {
      return {};
    }
  }
  return AggregatePartitions();
}

void SecEdgarQ4Module::AccumulateMutation(
    const core::MutationBatch &mutation, core::AuxEngineServices &,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ4TxnBuffer &>(base_buffer);
  if (mutation.table != config.num_table) {
    if (mutation.table == config.sub_table || mutation.table == config.tag_table ||
        mutation.table == config.pre_table) {
      buffer.MarkRebuild();
    }
    return;
  }
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    SecEdgarQ4NumInput parsed;
    if (!ReadString(row, 0, parsed.adsh) || !ReadString(row, 1, parsed.tag) ||
        !ReadString(row, 2, parsed.version) ||
        !ReadDouble(row, 7, parsed.value, parsed.value_is_null) ||
        !ReadString(row, 5, parsed.uom)) {
      buffer.MarkRebuild();
      return;
    }
    const bool relevant = parsed.uom == "USD" && !parsed.value_is_null;
    buffer.AddDelta({std::move(parsed.adsh),
                     {std::move(parsed.tag), std::move(parsed.version)},
                     parsed.value, sign, relevant});
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows) {
      add(row, 1);
    }
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows) {
      add(row, -1);
    }
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
SecEdgarQ4Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ4TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  if (buffer.Deltas().empty()) {
    return outcome;
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {core::SecEdgarQ4Layout::kSicCohort,
                   core::SecEdgarQ4Layout::kPreEqBridge,
                   core::SecEdgarQ4Layout::kTagTlabel,
                   core::SecEdgarQ4Layout::kGroupAggregates});
  SecEdgarQ4State state;
  core::Version aggregate_version = 0;
  bool have_cohort = false;
  bool have_bridge = false;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ4Layout::kSicCohort) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::SicCohort);
      if (typed) {
        state.sub_lookup = typed->state->sub_lookup;
        state.cik_dense_index = typed->state->cik_dense_index;
        state.cik_dense_count = typed->state->cik_dense_count;
        state.cik_bit_word_count = typed->state->cik_bit_word_count;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kPreEqBridge) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::PreEqBridge);
      if (typed) {
        state.pre_by_adsh = typed->state->pre_by_adsh;
        have_bridge = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kTagTlabel) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::TagTlabel);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        state.tag_version_index = typed->state->tag_version_index;
        state.tlabels = typed->state->tlabels;
        state.tlabel_ids_by_tag_version = typed->state->tlabel_ids_by_tag_version;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kGroupAggregates) {
      const auto *typed =
          AsQ4Image(image, SecEdgarQ4ImageKind::GroupAggregates);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
      aggregate_version = image.version;
    }
  }
  if (!have_cohort || !have_bridge || !have_tags || !have_aggregates ||
      state.group_keys.size() != state.aggregates.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &delta : buffer.Deltas()) {
    ApplyNumContribution(state, delta.adsh, delta.tag_version, delta.value,
                         delta.sign);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  const auto typed_state = std::make_shared<const SecEdgarQ4State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ4Layout::kAccelerator,
      core::SecEdgarQ4Layout::kGroupAggregates, aggregate_version,
      MakeStateImage(SecEdgarQ4ImageKind::GroupAggregates, typed_state)));
  return outcome;
}

bool SecEdgarQ4Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ4Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ4Layout::kTagTlabel,
       core::SecEdgarQ4Layout::kGroupAggregates});
  SecEdgarQ4State state;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ4Layout::kTagTlabel) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::TagTlabel);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        state.tlabel_ids_by_tag_version = typed->state->tlabel_ids_by_tag_version;
        state.tlabels = typed->state->tlabels;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kGroupAggregates) {
      const auto *typed =
          AsQ4Image(image, SecEdgarQ4ImageKind::GroupAggregates);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_tags || !have_aggregates ||
      state.group_keys.size() != state.aggregates.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ4Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ4State
SecEdgarQ4Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ4State state;
  if (!initial_scan) {
    throw std::logic_error(
        "SEC EDGAR Q4 requires a typed initial scan source");
  }
  BuildSubLookup(state, initial_scan);
  BuildTagLookup(state, initial_scan);
  BuildPreBridge(state, initial_scan);
  BuildAggregates(state, initial_scan);
  return state;
}

bool SecEdgarQ4Module::LoadState(core::AuxEngineServices &services,
                                 SecEdgarQ4State &state) const {
  const auto snapshot =
      services.StateStore().Read(services.CurrentVisibility());
  bool cohort = false;
  bool bridge = false;
  bool tags = false;
  bool aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ4Layout::kSicCohort) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::SicCohort);
      if (typed) {
        state.sub_lookup = typed->state->sub_lookup;
        state.cik_dense_index = typed->state->cik_dense_index;
        state.cik_dense_count = typed->state->cik_dense_count;
        state.cik_bit_word_count = typed->state->cik_bit_word_count;
        cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kPreEqBridge) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::PreEqBridge);
      if (typed) {
        state.pre_by_adsh = typed->state->pre_by_adsh;
        bridge = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kTagTlabel) {
      const auto *typed = AsQ4Image(image, SecEdgarQ4ImageKind::TagTlabel);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        state.tag_version_index = typed->state->tag_version_index;
        state.tlabels = typed->state->tlabels;
        state.tlabel_ids_by_tag_version = typed->state->tlabel_ids_by_tag_version;
        tags = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kGroupAggregates) {
      const auto *typed =
          AsQ4Image(image, SecEdgarQ4ImageKind::GroupAggregates);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        state.aggregates = typed->state->aggregates;
        aggregates = true;
      }
    }
  }
  return cohort && bridge && tags && aggregates &&
         state.group_keys.size() == state.aggregates.size();
}

void SecEdgarQ4Module::EmitOutput(const SecEdgarQ4State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"sic", core::AuxType::Int64, false},
               {"tlabel", core::AuxType::String, false},
               {"stmt", core::AuxType::String, false},
               {"num_companies", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false},
               {"avg_value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ4Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(row.sic)),
                    core::AuxValue::Of(core::AuxType::String, row.tlabel),
                    core::AuxValue::Of(core::AuxType::String, row.stmt),
                    core::AuxValue::Of(core::AuxType::Int64, row.num_companies),
                    core::AuxValue::Of(core::AuxType::Double, row.total_value),
                    core::AuxValue::Of(core::AuxType::Double, row.avg_value)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
