-- AFTER ROW mutation capture for Postgres aux invalidation.
-- Requires extension LOADed and C symbol accel_postgres_capture_mutation_trigger.

CREATE OR REPLACE FUNCTION accel_postgres_capture_mutation_trigger()
RETURNS trigger
AS 'accelerator_codegen_pg', 'accel_postgres_capture_mutation_trigger'
LANGUAGE C;

DO $$
DECLARE t text;
BEGIN
  FOREACH t IN ARRAY ARRAY[
    'lineitem','orders','customer','part','partsupp','supplier','nation','region'
  ]
  LOOP
    EXECUTE format('DROP TRIGGER IF EXISTS accel_pg_row_capture_i ON %I', t);
    EXECUTE format('DROP TRIGGER IF EXISTS accel_pg_row_capture_u ON %I', t);
    EXECUTE format('DROP TRIGGER IF EXISTS accel_pg_row_capture_d ON %I', t);
    EXECUTE format(
      'CREATE TRIGGER accel_pg_row_capture_i AFTER INSERT ON %I FOR EACH ROW '
      'EXECUTE FUNCTION accel_postgres_capture_mutation_trigger()', t);
    EXECUTE format(
      'CREATE TRIGGER accel_pg_row_capture_u AFTER UPDATE ON %I FOR EACH ROW '
      'EXECUTE FUNCTION accel_postgres_capture_mutation_trigger()', t);
    EXECUTE format(
      'CREATE TRIGGER accel_pg_row_capture_d AFTER DELETE ON %I FOR EACH ROW '
      'EXECUTE FUNCTION accel_postgres_capture_mutation_trigger()', t);
  END LOOP;
END $$;
