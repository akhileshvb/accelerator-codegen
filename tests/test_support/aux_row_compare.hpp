#pragma once

#include "accelerator_codegen/aux_state/core/aux_types.hpp"

namespace accelerator_codegen::test_support {

inline bool AuxValuesEqual(const aux_state::core::AuxValue &lhs,
                           const aux_state::core::AuxValue &rhs) {
  if (lhs.is_null != rhs.is_null)
    return false;
  if (lhs.is_null)
    return lhs.type == rhs.type;
  return lhs.payload.index() == rhs.payload.index() &&
         lhs.payload == rhs.payload;
}

inline bool AuxRowsEqual(const aux_state::core::AuxRow &lhs,
                         const aux_state::core::AuxRow &rhs) {
  if (lhs.values.size() != rhs.values.size())
    return false;
  for (std::size_t index = 0; index < lhs.values.size(); ++index) {
    if (!AuxValuesEqual(lhs.values[index], rhs.values[index]))
      return false;
  }
  return true;
}

} // namespace accelerator_codegen::test_support
