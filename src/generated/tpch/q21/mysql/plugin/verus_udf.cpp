// UDF symbols for the Verus-verified Q21 (only when the kernel is available).
#if defined(ACCEL_HAVE_Q21_VERUS)
#include "mysql/udf_shim.hpp"

ACCEL_MYSQL_QUERY_UDF(mysql_accel_tpch_q21_verus)
#endif
