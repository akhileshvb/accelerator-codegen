#include "sec_edgar/q4/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {
using accelerator_codegen::ChunkDouble;
using accelerator_codegen::ChunkInt32;
using accelerator_codegen::ChunkString;
using accelerator_codegen::ScanSecEdgarParallel;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarPreScan;
using accelerator_codegen::SecEdgarSubScan;
using accelerator_codegen::SecEdgarTagScan;

namespace {
constexpr int32_t kSicMin = 4000;
constexpr int32_t kSicMax = 4999;

const SecEdgarQ4StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ4ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ4StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
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

uint32_t InternTagVersion(SecEdgarQ4State &state, const SecEdgarQ4TagVersionKey &key,
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

void AccumulateGroup(SecEdgarQ4GroupAggregate &group, uint32_t cik_dense_id,
                     uint32_t cik_bit_word_count, uint32_t cik_dense_count,
                     double value, uint32_t multiplicity) {
  if (group.cik_bits.size() != cik_bit_word_count) {
    group.cik_bits.assign(cik_bit_word_count, 0);
  }
  group.sum += value * static_cast<double>(multiplicity);
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
                          const SecEdgarQ4TagVersionKey &tag_version, double value) {
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
  AccumulateGroup(state.aggregates[resolved.group_id], resolved.cik_dense_id,
                  state.cik_bit_word_count, state.cik_dense_count, value,
                  resolved.multiplicity);
}

void MergeAggregate(SecEdgarQ4GroupAggregate &dst, const SecEdgarQ4GroupAggregate &src,
                    uint32_t word_count) {
  dst.sum += src.sum;
  dst.count += src.count;
  if (dst.cik_bits.size() != word_count) {
    dst.cik_bits.assign(word_count, 0);
  }
  if (src.cik_bits.size() == word_count) {
    for (uint32_t i = 0; i < word_count; ++i) {
      dst.cik_bits[i] |= src.cik_bits[i];
    }
  }
  dst.distinct_cik_count = 0;
  for (uint32_t i = 0; i < dst.cik_bits.size(); ++i) {
    uint64_t word = dst.cik_bits[i];
    while (word) {
      dst.distinct_cik_count += static_cast<uint32_t>(word & 1U);
      word >>= 1;
    }
  }
}
} // namespace

void SecEdgarQ4ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ4ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ4ParallelModule::CheckValidity(core::AuxEngineServices &services,
                                             const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ4ParallelModule::Execute(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ4Layout::kTagTlabel,
                                     core::SecEdgarQ4Layout::kGroupAggregates});
  SecEdgarQ4State state;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ4Layout::kTagTlabel) {
      const auto *typed = AsImage(image, SecEdgarQ4ImageKind::TagTlabel);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        state.tlabel_ids_by_tag_version = typed->state->tlabel_ids_by_tag_version;
        state.tlabels = typed->state->tlabels;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ4Layout::kGroupAggregates) {
      const auto *typed = AsImage(image, SecEdgarQ4ImageKind::GroupAggregates);
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

void SecEdgarQ4ParallelModule::ExecuteFallback(core::AuxEngineServices &services,
                                               const core::QueryArguments &,
                                               core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ4State
SecEdgarQ4ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  SecEdgarQ4State state;

  std::vector<std::unordered_map<std::string, SecEdgarQ4SubInfo>> local_sub(workers);
  auto consume_sub = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_sub[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      const auto sic = ChunkInt32(chunk, 2, row);
      if (sic < kSicMin || sic > kSicMax) {
        continue;
      }
      auto &info = map[ChunkString(chunk, 0, row)];
      info.cik = ChunkInt32(chunk, 1, row);
      info.sic = sic;
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarSubScan({"adsh", "cik", "sic"}),
                            consume_sub)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q4 parallel requires a scan source");
    }
    initial_scan->ScanSub([&](const SecEdgarQ4SubInput &input) {
      if (input.sic < kSicMin || input.sic > kSicMax) {
        return;
      }
      auto &info = local_sub[0][input.adsh];
      info.cik = input.cik;
      info.sic = input.sic;
    });
  }
  for (auto &part : local_sub) {
    for (auto &[adsh, info] : part) {
      if (state.cik_dense_index.find(info.cik) == state.cik_dense_index.end()) {
        state.cik_dense_index[info.cik] = state.cik_dense_count++;
      }
      info.cik_dense_id = state.cik_dense_index[info.cik];
      state.sub_lookup.emplace(adsh, info);
    }
  }
  state.cik_bit_word_count =
      state.cik_dense_count == 0 ? 0 : (state.cik_dense_count + 63U) / 64U;

  const auto &sub_lookup = state.sub_lookup;
  std::vector<std::set<SecEdgarQ4TagVersionKey>> local_needed(workers);
  auto consume_needed = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &needed_local = local_needed[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "EQ") {
        continue;
      }
      if (sub_lookup.find(ChunkString(chunk, 0, row)) == sub_lookup.end()) {
        continue;
      }
      needed_local.insert({ChunkString(chunk, 1, row), ChunkString(chunk, 2, row)});
    }
  };
  const auto pre_spec = SecEdgarPreScan({"adsh", "tag", "version", "stmt"});
  if (!ScanSecEdgarParallel(services, pre_spec, consume_needed)) {
    initial_scan->ScanPre([&](const SecEdgarQ4PreInput &input) {
      if (input.stmt == "EQ" &&
          state.sub_lookup.find(input.adsh) != state.sub_lookup.end()) {
        local_needed[0].insert({input.tag, input.version});
      }
    });
  }
  std::set<SecEdgarQ4TagVersionKey> needed;
  for (const auto &part : local_needed) {
    needed.insert(part.begin(), part.end());
  }

  const auto &needed_keys = needed;
  std::vector<std::unordered_map<SecEdgarQ4TagVersionKey, std::string,
                                 SecEdgarQ4TagVersionKeyHash>>
      local_tags(workers);
  auto consume_tag = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &tags = local_tags[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkInt32(chunk, 2, row) != 0) {
        continue;
      }
      SecEdgarQ4TagVersionKey key{ChunkString(chunk, 0, row),
                                  ChunkString(chunk, 1, row)};
      if (needed_keys.find(key) == needed_keys.end()) {
        continue;
      }
      tags.emplace(std::move(key), ChunkString(chunk, 3, row));
    }
  };
  if (!ScanSecEdgarParallel(
          services, SecEdgarTagScan({"tag", "version", "abstract", "tlabel"}),
          consume_tag)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q4 parallel requires a scan source");
    }
    initial_scan->ScanTag([&](const SecEdgarQ4TagInput &input) {
      if (input.abstract_flag != 0 ||
          needed.find({input.tag, input.version}) == needed.end()) {
        return;
      }
      local_tags[0].emplace(SecEdgarQ4TagVersionKey{input.tag, input.version},
                            input.tlabel);
    });
  }
  for (auto &part : local_tags) {
    for (auto &[key, tlabel] : part) {
      InternTagVersion(state, key, InternTlabel(state, tlabel));
    }
  }

  const auto &tag_version_index = state.tag_version_index;
  std::vector<std::unordered_map<
      std::string, std::unordered_map<SecEdgarQ4TagVersionKey, uint32_t,
                                      SecEdgarQ4TagVersionKeyHash>>>
      local_pre(workers);
  auto consume_pre = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &pre = local_pre[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "EQ") {
        continue;
      }
      const auto adsh = ChunkString(chunk, 0, row);
      if (sub_lookup.find(adsh) == sub_lookup.end()) {
        continue;
      }
      SecEdgarQ4TagVersionKey key{ChunkString(chunk, 1, row),
                                  ChunkString(chunk, 2, row)};
      if (tag_version_index.find(key) == tag_version_index.end()) {
        continue;
      }
      pre[adsh][std::move(key)]++;
    }
  };
  if (!ScanSecEdgarParallel(services, pre_spec, consume_pre)) {
    initial_scan->ScanPre([&](const SecEdgarQ4PreInput &input) {
      if (input.stmt != "EQ") {
        return;
      }
      if (state.sub_lookup.find(input.adsh) == state.sub_lookup.end()) {
        return;
      }
      SecEdgarQ4TagVersionKey key{input.tag, input.version};
      if (state.tag_version_index.find(key) == state.tag_version_index.end()) {
        return;
      }
      local_pre[0][input.adsh][std::move(key)]++;
    });
  }
  for (auto &part : local_pre) {
    for (auto &[adsh, tags] : part) {
      const auto sub_it = state.sub_lookup.find(adsh);
      if (sub_it == state.sub_lookup.end()) {
        continue;
      }
      for (auto &[tv, multiplicity] : tags) {
        const auto tag_it = state.tag_version_index.find(tv);
        if (tag_it == state.tag_version_index.end()) {
          continue;
        }
        const uint32_t tlabel_id = state.tlabel_ids_by_tag_version[tag_it->second];
        const uint32_t group_id = InternGroup(state, sub_it->second.sic, tlabel_id);
        auto &resolved = state.pre_by_adsh[adsh][tag_it->second];
        resolved.group_id = group_id;
        resolved.cik_dense_id = sub_it->second.cik_dense_id;
        resolved.multiplicity += multiplicity;
      }
    }
  }

  std::vector<std::vector<SecEdgarQ4GroupAggregate>> local_agg(workers);
  for (auto &part : local_agg) {
    part = state.aggregates;
  }
  const auto &pre_by_adsh = state.pre_by_adsh;
  auto consume_num = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &aggs = local_agg[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "USD") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 4, row, value_null);
      if (value_null) {
        continue;
      }
      const auto adsh = ChunkString(chunk, 0, row);
      const auto sub_it = sub_lookup.find(adsh);
      if (sub_it == sub_lookup.end()) {
        continue;
      }
      const auto tag_it = tag_version_index.find(
          {ChunkString(chunk, 1, row), ChunkString(chunk, 2, row)});
      if (tag_it == tag_version_index.end()) {
        continue;
      }
      const auto pre_adsh_it = pre_by_adsh.find(adsh);
      if (pre_adsh_it == pre_by_adsh.end()) {
        continue;
      }
      const auto pre_entry_it = pre_adsh_it->second.find(tag_it->second);
      if (pre_entry_it == pre_adsh_it->second.end() ||
          pre_entry_it->second.multiplicity == 0 ||
          pre_entry_it->second.group_id >= aggs.size()) {
        continue;
      }
      const auto &resolved = pre_entry_it->second;
      AccumulateGroup(aggs[resolved.group_id], resolved.cik_dense_id,
                      state.cik_bit_word_count, state.cik_dense_count, value,
                      resolved.multiplicity);
    }
  };
  if (!ScanSecEdgarParallel(
          services, SecEdgarNumScan({"adsh", "tag", "version", "uom", "value"}),
          consume_num)) {
    initial_scan->ScanNum([&](const SecEdgarQ4NumInput &input) {
      if (input.uom != "USD" || input.value_is_null) {
        return;
      }
      ApplyNumContribution(state, input.adsh, {input.tag, input.version},
                           input.value);
    });
    return state;
  }
  for (const auto &part : local_agg) {
    for (std::size_t i = 0; i < part.size() && i < state.aggregates.size(); ++i) {
      MergeAggregate(state.aggregates[i], part[i], state.cik_bit_word_count);
    }
  }
  return state;
}

void SecEdgarQ4ParallelModule::EmitOutput(const SecEdgarQ4State &state,
                                          core::AuxRowSink &sink) const {
  sink.Begin({{{"sic", core::AuxType::Int64, false},
               {"tlabel", core::AuxType::String, false},
               {"stmt", core::AuxType::String, false},
               {"num_companies", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false},
               {"avg_value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ4Output(state)) {
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(row.sic)),
          core::AuxValue::Of(core::AuxType::String, row.tlabel),
          core::AuxValue::Of(core::AuxType::String, row.stmt),
          core::AuxValue::Of(core::AuxType::Int64, row.num_companies),
          core::AuxValue::Of(core::AuxType::Double, row.total_value),
          core::AuxValue::Of(core::AuxType::Double, row.avg_value)}});
  }
  sink.End();
}

void SecEdgarQ4ParallelModule::EmitState(const SecEdgarQ4State &state,
                                         core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ4State>(state);
  auto make = [&](SecEdgarQ4ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ4StateImage> {
    auto image = std::make_shared<SecEdgarQ4StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(kId, core::SecEdgarQ4Layout::kSicCohort, 0,
                                       make(SecEdgarQ4ImageKind::SicCohort)));
  builder.Write(core::StateImage::Make(kId, core::SecEdgarQ4Layout::kPreEqBridge, 0,
                                       make(SecEdgarQ4ImageKind::PreEqBridge)));
  builder.Write(core::StateImage::Make(kId, core::SecEdgarQ4Layout::kTagTlabel, 0,
                                       make(SecEdgarQ4ImageKind::TagTlabel)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ4Layout::kGroupAggregates, 0,
      make(SecEdgarQ4ImageKind::GroupAggregates)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
