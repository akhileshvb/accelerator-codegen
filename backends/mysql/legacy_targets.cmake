# MySQL eval harnesses (to be absorbed into backend.cmake): TPC-H aux differential / parameter-generality / isolation /
# calibration drivers. They use the mysql CLI for SQL connections (no libmysqlclient
# linking). Included by accel_include_backends after backend.cmake.

accel_add_backend_exe(mysql test_mysql_tpch_fuzz eval/tests/test_mysql_tpch_fuzz.cpp)
# all 22 queries; spec / off-spec / edge / NULL params; plain, data-perturb,
# wide-keys, negative-keys modes
accel_add_backend_exe(mysql test_mysql_tpch_params eval/tests/test_mysql_tpch_params.cpp)
accel_add_backend_exe(mysql test_mysql_tpch_differential eval/tests/test_mysql_tpch_differential.cpp)
foreach(_t IN ITEMS test_mysql_cross_accel test_mysql_aux_multi_connection_stress
                    test_mysql_aux_build_race test_mysql_aux_build_ddl_race test_mysql_aux_session_isolation
                    test_mysql_aux_read_after_commit test_mysql_tpch_rf_differential
                    benchmark_mysql_tpch_aux calibrate_mysql_tpch_aux_cost_model)
    if(_t MATCHES "^(benchmark|calibrate)_")
        accel_add_backend_exe(mysql ${_t} eval/bench/mysql/${_t}.cpp)
    else()
        accel_add_backend_exe(mysql ${_t} eval/tests/${_t}.cpp)
    endif()
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(test_mysql_aux_multi_connection_stress PRIVATE Threads::Threads)

if(BUILD_TESTING)
    add_test(NAME mysql_tpch_differential COMMAND test_mysql_tpch_differential)
    add_test(NAME test_mysql_cross_accel COMMAND test_mysql_cross_accel)
    add_test(NAME test_mysql_aux_multi_connection_stress COMMAND test_mysql_aux_multi_connection_stress)
endif()
