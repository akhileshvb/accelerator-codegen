// SCAFFOLDING
#include "accelerator_codegen/harness/aux_state_footprint.hpp"

#include <cstring>
#include <vector>

#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"
#include "sec_edgar/q1/aux/module.hpp"
#include "sec_edgar/q2/aux/module.hpp"
#include "sec_edgar/q3/aux/module.hpp"
#include "sec_edgar/q4/aux/module.hpp"
#include "sec_edgar/q6/aux/module.hpp"
#include "sec_edgar/q24/aux/module.hpp"
#include "tpch/q1/aux/module.hpp"
#include "tpch/q10/aux/module.hpp"
#include "tpch/q11/aux/module.hpp"
#include "tpch/q12/aux/module.hpp"
#include "tpch/q14/aux/module.hpp"
#include "tpch/q17/aux/module.hpp"
#include "tpch/q18/aux/module.hpp"
#include "tpch/q21/aux/module.hpp"
#include "tpch/q3/aux/module.hpp"
#include "tpch/q6/aux/module.hpp"
#include "tpch/q9/aux/module.hpp"

namespace accelerator_codegen::aux_state::harness {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using query::TpchQ1StateImage;
using query::TpchQ3ImageKind;
using query::TpchQ3StateImage;
using query::TpchQ6StateImage;
using query::TpchQ9ImageKind;
using query::TpchQ9StateImage;
using query::TpchQ10ImageKind;
using query::TpchQ10StateImage;
using query::TpchQ11ImageKind;
using query::TpchQ11StateImage;
using query::TpchQ12ImageKind;
using query::TpchQ12StateImage;
using query::TpchQ14ImageKind;
using query::TpchQ14StateImage;
using query::TpchQ17ImageKind;
using query::TpchQ17State;
using query::TpchQ17StateImage;
using query::TpchQ18ImageKind;
using query::TpchQ18StateImage;
using query::TpchQ21ImageKind;
using query::TpchQ21StateImage;
using query::SecEdgarQ1ImageKind;
using query::SecEdgarQ1StateImage;
using query::SecEdgarQ2ImageKind;
using query::SecEdgarQ2StateImage;
using query::SecEdgarQ3ImageKind;
using query::SecEdgarQ3StateImage;
using query::SecEdgarQ4ImageKind;
using query::SecEdgarQ4StateImage;
using query::SecEdgarQ6ImageKind;
using query::SecEdgarQ6StateImage;
using query::SecEdgarQ24ImageKind;
using query::SecEdgarQ24StateImage;

uint64_t AddEncoded(const std::vector<uint8_t> &bytes) {
  return bytes.size();
}

uint64_t EstimateTpchQ17AttrPartsBytes(const TpchQ17StateImage &image) {
  if (!image.attr_parts)
    return 0;
  uint64_t bytes = image.attr_parts->brand.size() +
                   image.attr_parts->container.size() + 32;
  bytes += image.attr_parts->contributions.size() * 16;
  return bytes;
}

uint64_t FootprintTpchQ1(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ1StateImage *>(payload.get());
    if (!image || !image->partition) {
      return;
    }
    total += AddEncoded(query::EncodeTpchQ1Day(*image->partition));
  });
  return total;
}

uint64_t FootprintTpchQ3(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ3StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ3ImageKind::OrderSegmentSide:
      if (image->order_segment_side)
        total += 16 + image->order_segment_side->order_segment.size();
      break;
    case TpchQ3ImageKind::SegmentIntervalIndex: {
      if (!image->state)
        break;
      const auto it = image->state->segment_intervals.find(image->segment);
      if (it != image->state->segment_intervals.end()) {
        uint64_t bytes = 32;
        for (const auto &order : it->second.orders) {
          bytes += 8 + 4 + 4 + 4 + order.buckets.size() * 12;
        }
        total += bytes;
      }
      break;
    }
    }
  });
  return total;
}

uint64_t FootprintTpchQ6(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ6StateImage *>(payload.get());
    if (!image || !image->matrix) {
      return;
    }
    total += AddEncoded(query::EncodeTpchQ6Matrix(*image->matrix));
  });
  return total;
}

uint64_t FootprintTpchQ9(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ9StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ9ImageKind::CompleteState:
    case TpchQ9ImageKind::Metadata: {
      // Estimate — metadata no longer stores maintenance-only side maps.
      const auto &s = *image->state;
      uint64_t bytes = 64;
      bytes += s.nation_names.size() * 32;
      for (const auto &entry : s.name_postings) {
        bytes += entry.first.size() + 8 + entry.second.size() * 8;
      }
      bytes += s.nation_emit_order.size() * 32;
      total += bytes;
      break;
    }
    case TpchQ9ImageKind::MaintenanceSide: {
      const auto &s = *image->state;
      uint64_t bytes = 64;
      bytes += s.supplier_nations.size() * 16;
      bytes += s.order_years.size() * 12;
      bytes += s.partsupp_costs.size() * 24;
      total += bytes;
      break;
    }
    case TpchQ9ImageKind::ProfitSlot: {
      const auto slot = image->profit_slot;
      const auto &partitions = image->state->profit_partitions;
      if (slot < partitions.size()) {
        uint64_t bytes = 32;
        for (const auto &entry : partitions[slot].parts) {
          bytes += 8 + entry.second.normalized_name.size() +
                   entry.second.cells.size() * 16;
        }
        total += bytes;
      }
      break;
    }
    }
  });
  return total;
}

uint64_t FootprintTpchQ10(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ10StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ10ImageKind::Side: {
      // Estimate — full EncodeTpchQ10Side sorts + copies all customer strings
      // and dominates SF20 init wall time without changing stored state.
      uint64_t bytes = 16;
      for (const auto &entry : image->state->customers) {
        const auto &c = entry.second;
        bytes += 8 + 8 + 4 + 4 + c.name.size() + c.address.size() + c.phone.size() +
                 c.comment.size() + 16;
      }
      for (const auto &entry : image->state->nations) {
        bytes += 4 + 4 + entry.second.size();
      }
      total += bytes;
      break;
    }
    case TpchQ10ImageKind::Orders:
      total += 16 + image->state->orders.size() * (8 + 8 + 4);
      break;
    case TpchQ10ImageKind::Month: {
      const auto it = image->state->month_revenue.find(image->month);
      const auto n =
          it == image->state->month_revenue.end() ? 0 : it->second.size();
      total += 16 + n * (8 + 8);
      break;
    }
    }
  });
  return total;
}

uint64_t FootprintTpchQ11(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ11StateImage *>(payload.get());
    if (!image) {
      return;
    }
    switch (image->kind) {
    case TpchQ11ImageKind::Metadata:
      if (image->state) {
        total += AddEncoded(query::EncodeTpchQ11Metadata(*image->state));
      }
      break;
    case TpchQ11ImageKind::NationPreAggregated:
      total += AddEncoded(query::EncodeTpchQ11NationPartition(
          image->nationkey, image->nation_name, image->total_value,
          image->sorted_rows));
      break;
    }
  });
  return total;
}

uint64_t FootprintTpchQ12(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ12StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ12ImageKind::OrderPriorities:
      // Estimate — avoid encoding ~30M order priorities at SF20 init.
      total += 16 + image->state->order_priorities.size() * (8 + 16);
      break;
    case TpchQ12ImageKind::Day:
      total += AddEncoded(
          query::EncodeTpchQ12Day(image->receipt_day, *image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintTpchQ14(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ14StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ14ImageKind::PartPromo:
      total += AddEncoded(query::EncodeTpchQ14PartPromo(*image->state));
      break;
    case TpchQ14ImageKind::Month:
      total += AddEncoded(
          query::EncodeTpchQ14Month(image->month, *image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintTpchQ17(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ17StateImage *>(payload.get());
    if (!image) {
      return;
    }
    switch (image->kind) {
    case TpchQ17ImageKind::PartStatsChunk:
      if (!image->chunk) {
        break;
      }
      {
        // Estimate — avoid per-part full encode loops at SF20 init.
        uint64_t bytes = 32;
        for (const auto &[partkey, entry] : image->chunk->parts) {
          (void)partkey;
          bytes += 8 + entry.attr_key.size() + 32;
        }
        total += bytes;
      }
      break;
    case TpchQ17ImageKind::AttrParts:
      total += EstimateTpchQ17AttrPartsBytes(*image);
      break;
    }
  });
  return total;
}

uint64_t FootprintTpchQ18(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ18StateImage *>(payload.get());
    if (!image || !image->state ||
        image->kind != TpchQ18ImageKind::OrderQuantitySums) {
      return;
    }
    // Estimate sparse survivors + dense quantity chunks.
    const auto &s = *image->state;
    uint64_t bytes = 64 + s.active_orderkeys.size() * 8;
    for (const auto &chunk : s.quantity_chunks)
      bytes += chunk.size() * 8;
    bytes += s.survivors.size() * (8 + 8 + 4 + 8 + 8);
    for (const auto &entry : s.customer_names)
      bytes += 8 + entry.second.size() + 16;
    total += bytes;
  });
  return total;
}

uint64_t FootprintTpchQ21(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const TpchQ21StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case TpchQ21ImageKind::SupplierMetadata:
      total += AddEncoded(query::EncodeTpchQ21SupplierMetadata(*image->state));
      break;
    case TpchQ21ImageKind::OrderSummaries:
      total += AddEncoded(query::EncodeTpchQ21OrderSummaries(*image->state));
      break;
    case TpchQ21ImageKind::NationNameIndex:
      total += AddEncoded(query::EncodeTpchQ21NationNameIndex(*image->state));
      break;
    case TpchQ21ImageKind::Nation:
      total += AddEncoded(
          query::EncodeTpchQ21Nation(image->nationkey, *image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ1(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ1StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ1ImageKind::GroupKeys:
      total += AddEncoded(query::EncodeSecEdgarQ1GroupKeys(*image->state));
      break;
    case SecEdgarQ1ImageKind::GroupAggregates:
      total += AddEncoded(query::EncodeSecEdgarQ1GroupAggregates(*image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ2(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ2StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ2ImageKind::MaxPureMap:
      total += AddEncoded(query::EncodeSecEdgarQ2MaxPureMap(*image->state));
      break;
    case SecEdgarQ2ImageKind::Fy2022Cohort:
      total += AddEncoded(query::EncodeSecEdgarQ2Fy2022Cohort(*image->state));
      break;
    case SecEdgarQ2ImageKind::Candidates:
      total += AddEncoded(query::EncodeSecEdgarQ2Candidates(*image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ3(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ3StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ3ImageKind::Fy2022Cohort:
      total += AddEncoded(query::EncodeSecEdgarQ3Fy2022Cohort(*image->state));
      break;
    case SecEdgarQ3ImageKind::CikTotals:
      total += AddEncoded(query::EncodeSecEdgarQ3CikTotals(*image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ4(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ4StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ4ImageKind::SicCohort:
      total += AddEncoded(query::EncodeSecEdgarQ4SicCohort(*image->state));
      break;
    case SecEdgarQ4ImageKind::PreEqBridge:
      total += AddEncoded(query::EncodeSecEdgarQ4PreEqBridge(*image->state));
      break;
    case SecEdgarQ4ImageKind::TagTlabel:
      total += AddEncoded(query::EncodeSecEdgarQ4TagTlabel(*image->state));
      break;
    case SecEdgarQ4ImageKind::GroupAggregates:
      total += AddEncoded(query::EncodeSecEdgarQ4GroupAggregates(*image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ6(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ6StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ6ImageKind::Fy2023Cohort:
      total += AddEncoded(query::EncodeSecEdgarQ6Fy2023Cohort(*image->state));
      break;
    case SecEdgarQ6ImageKind::PreIsBridge:
      total += AddEncoded(query::EncodeSecEdgarQ6PreIsBridge(*image->state));
      break;
    case SecEdgarQ6ImageKind::GroupAggregates:
      total += AddEncoded(query::EncodeSecEdgarQ6GroupAggregates(*image->state));
      break;
    }
  });
  return total;
}

uint64_t FootprintSecEdgarQ24(const PartitionedStateStore &store) {
  uint64_t total = 0;
  store.ForEachSlot([&](PartitionKey, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t) {
    if (status != PartitionStatus::Valid || !payload) {
      return;
    }
    const auto *image = static_cast<const SecEdgarQ24StateImage *>(payload.get());
    if (!image || !image->state) {
      return;
    }
    switch (image->kind) {
    case SecEdgarQ24ImageKind::PreMembership:
      total += AddEncoded(query::EncodeSecEdgarQ24PreMembership(*image->state));
      break;
    case SecEdgarQ24ImageKind::TagVersions:
      total += AddEncoded(query::EncodeSecEdgarQ24TagVersions(*image->state));
      break;
    case SecEdgarQ24ImageKind::Aggregates:
      total += AddEncoded(query::EncodeSecEdgarQ24Aggregates(*image->state));
      break;
    }
  });
  return total;
}

} // namespace

uint64_t ComputeAuxStateFootprintBytes(const PartitionedStateStore &store,
                                       const char *accelerator_id) {
  if (!accelerator_id) {
    return 0;
  }

#define DISPATCH(accel, fn)                                                    \
  if (std::strcmp(accelerator_id, accel) == 0) {                             \
    return fn(store);                                                          \
  }

  DISPATCH(core::TpchQ1Layout::kAccelerator, FootprintTpchQ1);
  DISPATCH(core::TpchQ3Layout::kAccelerator, FootprintTpchQ3);
  DISPATCH(core::TpchQ6Layout::kAccelerator, FootprintTpchQ6);
  DISPATCH(query::TpchQ9Layout::kAccelerator, FootprintTpchQ9);
  DISPATCH(core::TpchQ10Layout::kAccelerator, FootprintTpchQ10);
  DISPATCH(query::TpchQ11Layout::kAccelerator, FootprintTpchQ11);
  DISPATCH(core::TpchQ12Layout::kAccelerator, FootprintTpchQ12);
  DISPATCH(core::TpchQ14Layout::kAccelerator, FootprintTpchQ14);
  DISPATCH(query::TpchQ17Layout::kAccelerator, FootprintTpchQ17);
  DISPATCH(query::TpchQ18Layout::kAccelerator, FootprintTpchQ18);
  DISPATCH(query::TpchQ21Layout::kAccelerator, FootprintTpchQ21);
  DISPATCH(core::SecEdgarQ1Layout::kAccelerator, FootprintSecEdgarQ1);
  DISPATCH(core::SecEdgarQ2Layout::kAccelerator, FootprintSecEdgarQ2);
  DISPATCH(core::SecEdgarQ3Layout::kAccelerator, FootprintSecEdgarQ3);
  DISPATCH(core::SecEdgarQ4Layout::kAccelerator, FootprintSecEdgarQ4);
  DISPATCH(core::SecEdgarQ6Layout::kAccelerator, FootprintSecEdgarQ6);
  DISPATCH(core::SecEdgarQ24Layout::kAccelerator, FootprintSecEdgarQ24);

#undef DISPATCH

  return 0;
}

} // namespace accelerator_codegen::aux_state::harness
