// GENERATED BACKEND
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// PostgreSQL shared memory cannot retain C++ object pointers. These records are
// the serialized boundary owned by the PostgreSQL adapter; the common state
// store remains typed after deserialization.
class PostgresStateStore final : public core::AuxStateStore {
public:
  struct SerializedStateImage {
    core::AcceleratorId accelerator;
    core::PartitionId partition = 0;
    core::Version version = 0;
    std::vector<uint8_t> bytes;
  };

  struct SerializedStateSnapshot {
    core::AcceleratorId accelerator;
    core::VisibilityToken visibility;
    std::vector<SerializedStateImage> partitions;
    bool available = false;
  };

  using ReadCallback =
      std::function<SerializedStateSnapshot(const core::VisibilityToken &)>;
  using DeserializeCallback =
      std::function<core::StateImage(const SerializedStateImage &)>;
  using SerializeCallback =
      std::function<SerializedStateImage(const core::StateImage &)>;
  using ValidityCallback = std::function<core::ValidityResult(
      const core::QueryKey &, const core::VisibilityToken &)>;
  using InvalidateCallback =
      std::function<void(const core::InvalidationPlan &)>;
  using PublishCallback = std::function<bool(
      const std::vector<SerializedStateImage> &, core::Version)>;

  PostgresStateStore(ReadCallback read, DeserializeCallback deserialize,
                     SerializeCallback serialize, ValidityCallback validity,
                     InvalidateCallback invalidate, PublishCallback publish);

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::ValidityResult
  CheckValidity(const core::QueryKey &,
                const core::VisibilityToken &) const override;
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

private:
  ReadCallback read;
  DeserializeCallback deserialize;
  SerializeCallback serialize;
  ValidityCallback validity;
  InvalidateCallback invalidate;
  PublishCallback publish;
};

} // namespace accelerator_codegen::aux_state::engine::postgres
