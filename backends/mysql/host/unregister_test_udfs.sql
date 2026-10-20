-- Remove the gate-test UDFs (backends/mysql/host/register_test_udfs.sql) for the
-- production plugin variant, which does not export them. Run after mysqld
-- restarted on the production .so: the functions then failed to load, so
-- DROP FUNCTION reports "does not exist" and leaves their mysql.func rows
-- (mysqld would log "Can't find symbol" for each on every start). Deleting
-- the rows works whether or not they are loaded.
DELETE FROM mysql.func WHERE name IN (
  'mysql_accel_txn_init',
  'mysql_accel_txn_capture',
  'mysql_accel_txn_read',
  'mysql_accel_txn_epoch',
  'mysql_accel_txn_reset',
  'mysql_accel_mutations_last',
  'mysql_accel_mutations_drain',
  'mysql_accel_mutations_count',
  'mysql_accel_scan_init',
  'mysql_accel_scan_exec',
  'mysql_accel_scan_proj',
  'mysql_accel_scan_filter',
  'mysql_accel_scan_clear',
  'mysql_accel_runtime_init',
  'mysql_accel_runtime_build',
  'mysql_accel_runtime_execute',
  'mysql_accel_runtime_read',
  'mysql_accel_runtime_reset'
);
