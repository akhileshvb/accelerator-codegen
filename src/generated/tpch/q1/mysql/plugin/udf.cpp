// UDF symbols for TPC-H Q1 (MySQL resolves UDFs by exported symbol name).
// The behavior is the generic UDF implementation in the backend plugin; the
// query is found by name in the UDF registry (see runtime.cpp here).
#include "mysql/udf_shim.hpp"

ACCEL_MYSQL_QUERY_UDF(mysql_accel_tpch_q1_aux)
