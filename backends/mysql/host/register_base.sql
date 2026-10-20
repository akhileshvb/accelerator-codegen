-- Register the backend (query-independent) MySQL UDFs and the plugin.
-- Per-query UDFs are registered from accelerators/<bench>/q<N>/mysql/plugin/functions.sql;
-- backends/mysql/host.sh build assembles base + fragments.
INSTALL PLUGIN accelerator_codegen_audit SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_observer_mutation_count;
CREATE FUNCTION mysql_accel_observer_mutation_count RETURNS INTEGER SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_reset_observer;
CREATE FUNCTION mysql_accel_reset_observer RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_set_force_maintenance_action;
CREATE FUNCTION accel_set_force_maintenance_action RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_get_last_maintenance_decision;
CREATE FUNCTION accel_get_last_maintenance_decision RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_invalidate_all;
CREATE FUNCTION accel_invalidate_all RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_set_scan_database;
CREATE FUNCTION mysql_accel_set_scan_database RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_set_scan_mode;
CREATE FUNCTION mysql_accel_set_scan_mode RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_get_build_stats;
CREATE FUNCTION accel_get_build_stats RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_get_execute_counts;
CREATE FUNCTION accel_get_execute_counts RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_set_expected_queries;
CREATE FUNCTION mysql_accel_set_expected_queries RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS accel_get_last_maintenance_action;
CREATE FUNCTION accel_get_last_maintenance_action RETURNS STRING SONAME 'accelerator_codegen_mysql.so';

DROP FUNCTION IF EXISTS mysql_accel_capture_row;
CREATE FUNCTION mysql_accel_capture_row RETURNS STRING SONAME 'accelerator_codegen_mysql.so';


