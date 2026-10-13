// SCAFFOLDING
#pragma once

#include <algorithm>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

inline std::size_t MutationRowCount(const MutationBatch &mutation) {
  switch (mutation.kind) {
  case MutationKind::Insert:
    return mutation.new_rows.size();
  case MutationKind::Delete:
    return mutation.old_rows.size();
  case MutationKind::Update:
    return std::max(mutation.old_rows.size(), mutation.new_rows.size());
  }
  return 0;
}

} // namespace accelerator_codegen::aux_state::core
