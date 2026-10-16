#pragma once

// The store-protocol switch for Verus accelerators.
//
// A Verus accelerator's state can live in either store:
//   * the C++ VersionedAuxStore (the default): the module hands it opaque
//     payloads, as every C++ accelerator does;
//   * its verified Verus store (substrate/verus/store): the versions, the
//     pending commit and the snapshot -> version resolution are in Verus,
//     and it is proved that every kept version's state answers for the
//     database at that version.
// Both implement core::AuxStateStore, so the runtime and the engine
// drive them the same way; the module prepares Verus-store versions itself
// when its services' store is one (see tpch/q21/verus/store.hpp).
//
// ACCEL_AUX_STORE=verus selects the Verus store for accelerators that have
// one; anything else (or unset) keeps the C++ store.  An engine runtime
// chooses once, when it creates the accelerator's store.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state {

class VerusAuxStore : public core::AuxStateStore {
public:
  using Epoch = core::Version;
  // As VersionedAuxStore's: the epoch of a commit (and of its pending mark),
  // and the oldest snapshot epoch in use (none: keep only the newest).
  using EpochProvider = std::function<Epoch()>;
  using FloorProvider = std::function<Epoch()>;

  // A fresh build (the module's Build images), current from `from_epoch`
  // (VersionedAuxStore::Install).
  virtual void Install(std::vector<core::StateImage> images,
                       const core::AcceleratorId &accelerator, Epoch from_epoch) = 0;

  virtual std::size_t ValidPartitionCount() const = 0;
  virtual std::size_t TotalPartitionCount() const = 0;
  virtual std::size_t TotalVersionCount() const = 0;
  // Installs that found a newer version than the one they were prepared
  // from (the store then holds one invalid version).  Should stay 0.
  virtual std::size_t StaleInstalls() const = 0;
};

// Whether ACCEL_AUX_STORE asks for the Verus store.
bool VerusAuxStoreRequested();

// The Verus store of `accelerator`, or null if it has none: a Verus kit
// accelerator's, or one a query registered below.
std::unique_ptr<VerusAuxStore> MakeVerusAuxStore(const core::AcceleratorId &accelerator,
                                                 VerusAuxStore::EpochProvider epoch,
                                                 VerusAuxStore::FloorProvider floor);

// A hand-written Verus accelerator's store, registered from the query's
// directory with a static VerusAuxStoreRegistrar.
using VerusAuxStoreFactory = std::unique_ptr<VerusAuxStore> (*)(VerusAuxStore::EpochProvider epoch,
                                                                VerusAuxStore::FloorProvider floor);
void RegisterVerusAuxStore(std::string accelerator, VerusAuxStoreFactory factory);
struct VerusAuxStoreRegistrar {
  VerusAuxStoreRegistrar(std::string accelerator, VerusAuxStoreFactory factory) {
    RegisterVerusAuxStore(std::move(accelerator), factory);
  }
};

} // namespace accelerator_codegen::aux_state
