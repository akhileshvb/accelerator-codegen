-- Register MySQL TPC-H aux UDFs (plugin SONAME accelerator_codegen_mysql.so).
INSTALL PLUGIN accelerator_codegen_audit SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q1_aux;
CREATE FUNCTION mysql_accel_tpch_q1_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q2_aux;
CREATE FUNCTION mysql_accel_tpch_q2_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q3_aux;
CREATE FUNCTION mysql_accel_tpch_q3_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q4_aux;
CREATE FUNCTION mysql_accel_tpch_q4_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q5_aux;
CREATE FUNCTION mysql_accel_tpch_q5_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q6_aux;
CREATE FUNCTION mysql_accel_tpch_q6_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q7_aux;
CREATE FUNCTION mysql_accel_tpch_q7_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q8_aux;
CREATE FUNCTION mysql_accel_tpch_q8_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q9_aux;
CREATE FUNCTION mysql_accel_tpch_q9_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q10_aux;
CREATE FUNCTION mysql_accel_tpch_q10_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q11_aux;
CREATE FUNCTION mysql_accel_tpch_q11_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q12_aux;
CREATE FUNCTION mysql_accel_tpch_q12_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q13_aux;
CREATE FUNCTION mysql_accel_tpch_q13_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q14_aux;
CREATE FUNCTION mysql_accel_tpch_q14_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q15_aux;
CREATE FUNCTION mysql_accel_tpch_q15_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q16_aux;
CREATE FUNCTION mysql_accel_tpch_q16_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q17_aux;
CREATE FUNCTION mysql_accel_tpch_q17_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q18_aux;
CREATE FUNCTION mysql_accel_tpch_q18_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q19_aux;
CREATE FUNCTION mysql_accel_tpch_q19_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q20_aux;
CREATE FUNCTION mysql_accel_tpch_q20_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q21_aux;
CREATE FUNCTION mysql_accel_tpch_q21_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_tpch_q22_aux;
CREATE FUNCTION mysql_accel_tpch_q22_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_observer_mutation_count;
CREATE FUNCTION mysql_accel_observer_mutation_count RETURNS INTEGER SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_reset_observer;
CREATE FUNCTION mysql_accel_reset_observer RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_set_force_maintenance_action;
CREATE FUNCTION mysql_accel_set_force_maintenance_action RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_get_last_maintenance_decision;
CREATE FUNCTION mysql_accel_get_last_maintenance_decision RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_get_execute_counts;
CREATE FUNCTION mysql_accel_get_execute_counts RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_set_expected_queries;
CREATE FUNCTION mysql_accel_set_expected_queries RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_get_last_maintenance_action;
CREATE FUNCTION mysql_accel_get_last_maintenance_action RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_mutations_capture_row;
CREATE FUNCTION mysql_accel_mutations_capture_row RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

