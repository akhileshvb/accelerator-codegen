// GENERATED BACKEND - serialized_boundary store
#pragma once

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

// PostgreSQL operates per-backend-process. The PostgresStateStore uses
// a file-backed PartitionedStateStore for cross-backend visibility.
// This is the serialized_boundary model.
class PostgresStateStore final : public core::AuxStateStore {
public:
  PostgresStateStore();
  explicit PostgresStateStore(std::string backing_dir);
  ~PostgresStateStore() override;

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

  // Contract test support: get the current mutation epoch.
  core::Version GetCurrentMutationEpoch() const;
  // Force load from backing files (for cross-backend visibility).
  void EnsureLoaded();

private:
  void LoadFromBacking();
  void SaveToBacking() const;

  std::string backing_dir;
  std::string GetBackingPath() const;
  std::string GetMutationEpochPath() const;
  std::string PartitionPath(PartitionKey key) const;

  mutable std::recursive_mutex store_mutex;
  mutable PartitionedStateStore store;
  core::Version mutation_epoch = 0;
};

// Toy payload for contract testing: serialized key-value.
struct ToySerializedPayload {
  std::string key;
  int64_t value = 0;

  std::vector<uint8_t> Encode() const;
  static ToySerializedPayload Decode(const std::vector<uint8_t> &data);
};

std::vector<uint8_t> EncodeToyPayload(const std::string &key, int64_t value);
std::pair<std::string, int64_t> DecodeToyPayload(const std::vector<uint8_t> &data);

} // namespace accelerator_codegen::aux_state::engine::postgres
