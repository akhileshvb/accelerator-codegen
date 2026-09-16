#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accel_cbmc {

using namespace accelerator_codegen::aux_state::core;

// Minimal AuxStateStore for headless CBMC harnesses. ReadPartitions only returns
// published images (intersection with the request), avoiding work proportional
// to a full TPC-H day span when the request list is large.
class InMemoryStore final : public AuxStateStore {
public:
  explicit InMemoryStore(std::string accelerator_id)
      : accelerator(std::move(accelerator_id)) {}

  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = accelerator;
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }

  StateSnapshot
  ReadPartitions(const VisibilityToken &visibility,
                 const std::vector<PartitionId> &partitions) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = accelerator;
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &partition : partitions) {
      auto it = images.find(partition);
      if (it != images.end())
        snapshot.partitions.push_back(it->second);
    }
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }

  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == accelerator && valid
               ? ValidityResult{ValidityCode::Valid, version}
               : ValidityResult{ValidityCode::Invalidated, 0};
  }

  void Invalidate(const InvalidationPlan &) override { valid = false; }

  bool TryPublish(const StagedMaintenanceOutcome &outcome,
                  Version expected_version) override {
    (void)expected_version;
    for (const auto &image : outcome.updates) {
      auto it = images.find(image.partition);
      if (it != images.end() && image.version != 0 &&
          image.version != it->second.version) {
        return false;
      }
    }
    for (const auto &image : outcome.updates) {
      auto &published = images[image.partition];
      published = image;
      if (published.version == 0)
        published.version = 1;
      else
        published.version++;
    }
    valid = !images.empty();
    version++;
    return true;
  }

  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.emplace(image.partition, std::move(image));
    }
    valid = !images.empty();
    version = 1;
  }

  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
  std::string accelerator;
};

class CollectingBuilder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

} // namespace accel_cbmc
