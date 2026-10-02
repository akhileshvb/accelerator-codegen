DROP FUNCTION IF EXISTS mysql_accel_tpch_q14_aux;
CREATE FUNCTION mysql_accel_tpch_q14_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
