-- Gate-test UDFs of the MySQL plugin TEST variant (built with
-- -DACCEL_MYSQL_TEST_UDFS=ON; backends/mysql/tests/mysql_bridge_test_udfs.cpp).
-- The production plugin does not export these: never register them against it.
-- host.sh build applies this file when
-- ACCEL_MYSQL_PLUGIN_VARIANT=test and backends/mysql/host/unregister_test_udfs.sql
-- otherwise.

DROP FUNCTION IF EXISTS mysql_accel_txn_init;
CREATE FUNCTION mysql_accel_txn_init RETURNS INTEGER SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_txn_capture;
CREATE FUNCTION mysql_accel_txn_capture RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_txn_read;
CREATE FUNCTION mysql_accel_txn_read RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_txn_epoch;
CREATE FUNCTION mysql_accel_txn_epoch RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_txn_reset;
CREATE FUNCTION mysql_accel_txn_reset RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_mutations_last;
CREATE FUNCTION mysql_accel_mutations_last RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_mutations_drain;
CREATE FUNCTION mysql_accel_mutations_drain RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_mutations_count;
CREATE FUNCTION mysql_accel_mutations_count RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_scan_init;
CREATE FUNCTION mysql_accel_scan_init RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_scan_exec;
CREATE FUNCTION mysql_accel_scan_exec RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_scan_proj;
CREATE FUNCTION mysql_accel_scan_proj RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_scan_filter;
CREATE FUNCTION mysql_accel_scan_filter RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_scan_clear;
CREATE FUNCTION mysql_accel_scan_clear RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_runtime_init;
CREATE FUNCTION mysql_accel_runtime_init RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_runtime_build;
CREATE FUNCTION mysql_accel_runtime_build RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_runtime_execute;
CREATE FUNCTION mysql_accel_runtime_execute RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_runtime_read;
CREATE FUNCTION mysql_accel_runtime_read RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
DROP FUNCTION IF EXISTS mysql_accel_runtime_reset;
CREATE FUNCTION mysql_accel_runtime_reset RETURNS STRING SONAME 'accelerator_codegen_mysql.so';
