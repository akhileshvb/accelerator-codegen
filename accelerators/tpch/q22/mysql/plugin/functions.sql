DROP FUNCTION IF EXISTS mysql_accel_tpch_q22_aux;
CREATE FUNCTION mysql_accel_tpch_q22_aux RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
