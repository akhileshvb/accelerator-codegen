# SEC-EDGAR Q2 PostgreSQL client tests: libpq only, run against a live instance
# that has the accelerator extension installed (PGHOST/PGPORT/... environment,
# default 127.0.0.1:55402).  Not registered with ctest (they need a server).
find_library(ACCEL_SEC_EDGAR_Q2_LIBPQ pq HINTS "${ACCEL_PG_LIBDIR}")
find_path(ACCEL_SEC_EDGAR_Q2_LIBPQ_INCLUDE libpq-fe.h HINTS "${ACCEL_PG_INCLUDEDIR}")
if(ACCEL_SEC_EDGAR_Q2_LIBPQ AND ACCEL_SEC_EDGAR_Q2_LIBPQ_INCLUDE)
  foreach(_t differential params)
    set(_src tests/aux_state/test_postgres_sec_edgar_q2_${_t}.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
      add_executable(test_postgres_sec_edgar_q2_${_t} ${_src})
      target_link_libraries(test_postgres_sec_edgar_q2_${_t} PRIVATE ${ACCEL_SEC_EDGAR_Q2_LIBPQ})
      target_include_directories(test_postgres_sec_edgar_q2_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests ${ACCEL_SEC_EDGAR_Q2_LIBPQ_INCLUDE})
    endif()
  endforeach()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/postgres/benchmark_postgres_sec_edgar_q2.cpp")
    add_executable(benchmark_postgres_sec_edgar_q2 benchmarks/postgres/benchmark_postgres_sec_edgar_q2.cpp)
    target_link_libraries(benchmark_postgres_sec_edgar_q2 PRIVATE ${ACCEL_SEC_EDGAR_Q2_LIBPQ})
    target_include_directories(benchmark_postgres_sec_edgar_q2 PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests ${ACCEL_SEC_EDGAR_Q2_LIBPQ_INCLUDE})
  endif()
endif()
