DROP FUNCTION IF EXISTS mysql_accel_tpch_q2_aux;
CREATE FUNCTION mysql_accel_tpch_q2_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
