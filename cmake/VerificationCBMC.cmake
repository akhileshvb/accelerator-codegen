# Optional CBMC verification targets for Postgres aux kernels.
# Skipped when cbmc/goto-cc are not on PATH.

find_program(ACCEL_CBMC cbmc)
find_program(ACCEL_GOTO_CC goto-cc)

if(ACCEL_CBMC AND ACCEL_GOTO_CC)
  message(STATUS "accel: CBMC found (${ACCEL_CBMC}); adding verify_postgres_*_aux targets")
  add_custom_target(verify_postgres_q1_aux
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_cbmc_postgres_aux.sh q1 all cbmc
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "CBMC bounded verification of Postgres TPC-H Q1 aux"
    VERBATIM
  )
  add_custom_target(verify_postgres_q6_aux
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_cbmc_postgres_aux.sh q6 all cbmc
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "CBMC bounded verification of Postgres TPC-H Q6 aux"
    VERBATIM
  )
  add_custom_target(verify_postgres_aux
    DEPENDS verify_postgres_q1_aux verify_postgres_q6_aux
  )
  add_custom_target(verify_postgres_q1_aux_concrete
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_cbmc_postgres_aux.sh q1 all concrete
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Concrete smoke of Postgres TPC-H Q1 aux verification harnesses"
    VERBATIM
  )
else()
  message(STATUS "accel: CBMC/goto-cc not found — skipping verify_postgres_*_aux targets "
                 "(install CBMC or run scripts/run_cbmc_postgres_aux.sh … concrete)")
endif()
