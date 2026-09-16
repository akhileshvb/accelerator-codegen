#pragma once

#include <cstdint>

// CBMC provides nondet_*; concrete smoke uses fixed harness values and stubs.
#if defined(__CPROVER__)
extern "C" {
int nondet_int(void);
long nondet_long(void);
unsigned nondet_unsigned(void);
char nondet_char(void);
}
#define ACCEL_CBMC_ASSUME(cond) __CPROVER_assume(cond)
#elif defined(ACCEL_VERIFICATION_CONCRETE)
inline int nondet_int(void) { return 0; }
inline long nondet_long(void) { return 0; }
inline unsigned nondet_unsigned(void) { return 0; }
inline char nondet_char(void) { return 0; }
#define ACCEL_CBMC_ASSUME(cond)                                                \
  do {                                                                         \
    if (!(cond))                                                               \
      __builtin_trap();                                                        \
  } while (0)
#else
inline int nondet_int(void) { return 0; }
inline long nondet_long(void) { return 0; }
inline unsigned nondet_unsigned(void) { return 0; }
inline char nondet_char(void) { return 0; }
#define ACCEL_CBMC_ASSUME(cond)                                                \
  do {                                                                         \
    if (!(cond))                                                               \
      __builtin_unreachable();                                                 \
  } while (0)
#endif

#ifndef __CPROVER_assume
#define __CPROVER_assume(cond) ACCEL_CBMC_ASSUME(cond)
#endif

namespace accel_cbmc {

inline int32_t NondetI32() { return static_cast<int32_t>(nondet_int()); }
inline int64_t NondetI64() { return static_cast<int64_t>(nondet_long()); }
inline int8_t NondetI8() { return static_cast<int8_t>(nondet_char()); }

inline void Assume(bool cond) { __CPROVER_assume(cond); }

} // namespace accel_cbmc
