# SEC-EDGAR Q2 PostgreSQL client tests: libpq only, run against a live instance
# that has the accelerator extension installed (PGHOST/PGPORT/... environment,
# default 127.0.0.1:55402).  Not registered with ctest (they need a server).
foreach(_t differential params)
  accel_add_backend_exe(postgres test_postgres_sec_edgar_q2_${_t} CLIENT
    accelerators/sec_edgar/q2/tests/test_postgres_sec_edgar_q2_${_t}.cpp)
endforeach()
accel_add_backend_exe(postgres benchmark_postgres_sec_edgar_q2 CLIENT
  accelerators/sec_edgar/q2/bench/benchmark_postgres_sec_edgar_q2.cpp)
