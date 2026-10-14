#pragma once
#include <cstddef>
namespace accelerator_codegen::aux_state::core {
struct ScopedTimer {
  explicit ScopedTimer(double *, const char * = nullptr) {}
  inline void Reset(double * = nullptr) {}
};
}
#define ACCEL_MEASURE_SCOPE(phase) (void)(phase)
