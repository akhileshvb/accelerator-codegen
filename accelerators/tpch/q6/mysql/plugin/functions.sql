DROP FUNCTION IF EXISTS mysql_accel_tpch_q6_aux;
CREATE FUNCTION mysql_accel_tpch_q6_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
