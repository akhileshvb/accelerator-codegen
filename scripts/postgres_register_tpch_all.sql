-- The generated deployment layer creates one statement-level trigger per
-- dependent table and operation. Transition tables are read-only relations:
--
--   AFTER INSERT ON t REFERENCING NEW TABLE AS new_rows
--   AFTER DELETE ON t REFERENCING OLD TABLE AS old_rows
--   AFTER UPDATE ON t REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
--
-- The trigger decoder maps those rows to MutationBatch before calling the
-- backend-local PostgresMutationCapture. Keeping trigger creation generated
-- avoids hard-coding table OIDs or query-specific column layouts here.

DROP FUNCTION IF EXISTS accel_aux_capture_transition CASCADE;
CREATE FUNCTION accel_aux_capture_transition()
RETURNS trigger
AS 'accelerator_codegen_pg', 'AcceleratorCodegenCaptureTransition'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_get_observer_mutation_count() CASCADE;
CREATE FUNCTION accel_postgres_get_observer_mutation_count()
RETURNS bigint
AS 'accelerator_codegen_pg', 'accel_postgres_get_observer_mutation_count'
LANGUAGE C STRICT;

DROP FUNCTION IF EXISTS accel_postgres_reset_observer() CASCADE;
CREATE FUNCTION accel_postgres_reset_observer()
RETURNS void
AS 'accelerator_codegen_pg', 'accel_postgres_reset_observer'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_set_force_maintenance_action(text) CASCADE;
CREATE FUNCTION accel_postgres_set_force_maintenance_action(text)
RETURNS void
AS 'accelerator_codegen_pg', 'accel_postgres_set_force_maintenance_action'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_get_last_maintenance_action() CASCADE;
CREATE FUNCTION accel_postgres_get_last_maintenance_action()
RETURNS text
AS 'accelerator_codegen_pg', 'accel_postgres_get_last_maintenance_action'
LANGUAGE C STRICT;

-- Base codegen kernels (stateless native SPI scans)
DROP FUNCTION IF EXISTS accel_tpch_q1 CASCADE;
CREATE FUNCTION accel_tpch_q1(integer)
RETURNS TABLE (
  l_returnflag text,
  l_linestatus text,
  sum_qty numeric,
  sum_base_price numeric,
  sum_disc_price numeric,
  sum_charge numeric,
  avg_qty double precision,
  avg_price double precision,
  avg_disc double precision,
  count_order bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ1'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q3 CASCADE;
CREATE FUNCTION accel_tpch_q3(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ3'
LANGUAGE C;

DROP TYPE IF EXISTS accel_tpch_q6_row CASCADE;
CREATE TYPE accel_tpch_q6_row AS (revenue numeric);

DROP FUNCTION IF EXISTS accel_tpch_q6 CASCADE;
CREATE FUNCTION accel_tpch_q6(date, double precision, double precision)
RETURNS SETOF accel_tpch_q6_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ6'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q10 CASCADE;
CREATE FUNCTION accel_tpch_q10(date)
RETURNS TABLE (
  c_custkey bigint,
  c_name text,
  revenue numeric,
  c_acctbal numeric,
  n_name text,
  c_address text,
  c_phone text,
  c_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ10'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q12 CASCADE;
CREATE FUNCTION accel_tpch_q12(text, text, date)
RETURNS TABLE (
  l_shipmode text,
  high_line_count bigint,
  low_line_count bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ12'
LANGUAGE C;

DROP TYPE IF EXISTS accel_tpch_q14_row CASCADE;
CREATE TYPE accel_tpch_q14_row AS (promo_revenue double precision);

DROP FUNCTION IF EXISTS accel_tpch_q14 CASCADE;
CREATE FUNCTION accel_tpch_q14(date)
RETURNS SETOF accel_tpch_q14_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ14'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q2 CASCADE;
CREATE FUNCTION accel_tpch_q2(integer, text, text)
RETURNS TABLE (
  s_acctbal numeric,
  s_name text,
  n_name text,
  p_partkey bigint,
  p_mfgr text,
  s_address text,
  s_phone text,
  s_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ2'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q4 CASCADE;
CREATE FUNCTION accel_tpch_q4(date)
RETURNS TABLE (
  o_orderpriority text,
  order_count bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ4'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q5 CASCADE;
CREATE FUNCTION accel_tpch_q5(text, date)
RETURNS TABLE (
  n_name text,
  revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ5'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q7 CASCADE;
CREATE FUNCTION accel_tpch_q7(text, text)
RETURNS TABLE (
  supp_nation text,
  cust_nation text,
  l_year bigint,
  revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ7'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q8 CASCADE;
CREATE FUNCTION accel_tpch_q8(text, text, text)
RETURNS TABLE (
  o_year bigint,
  mkt_share double precision
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ8'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q9 CASCADE;
CREATE FUNCTION accel_tpch_q9(text)
RETURNS TABLE (
  nation text,
  o_year bigint,
  sum_profit numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ9'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q11 CASCADE;
CREATE FUNCTION accel_tpch_q11(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ11'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q13 CASCADE;
CREATE FUNCTION accel_tpch_q13(text)
RETURNS TABLE (
  c_count bigint,
  custdist bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ13'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q15 CASCADE;
CREATE FUNCTION accel_tpch_q15(date)
RETURNS TABLE (
  s_suppkey bigint,
  s_name text,
  s_address text,
  s_phone text,
  total_revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ15'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q16 CASCADE;
CREATE FUNCTION accel_tpch_q16(text, text, integer, integer, integer, integer, integer, integer, integer, integer)
RETURNS TABLE (
  p_brand text,
  p_type text,
  p_size integer,
  supplier_cnt bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ16'
LANGUAGE C;

DROP TYPE IF EXISTS accel_tpch_q17_row CASCADE;
CREATE TYPE accel_tpch_q17_row AS (avg_yearly double precision);

DROP FUNCTION IF EXISTS accel_tpch_q17 CASCADE;
CREATE FUNCTION accel_tpch_q17(text, text)
RETURNS SETOF accel_tpch_q17_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ17'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q18 CASCADE;
CREATE FUNCTION accel_tpch_q18(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ18'
LANGUAGE C;

DROP TYPE IF EXISTS accel_tpch_q19_row CASCADE;
CREATE TYPE accel_tpch_q19_row AS (revenue numeric);

DROP FUNCTION IF EXISTS accel_tpch_q19 CASCADE;
CREATE FUNCTION accel_tpch_q19(text, bigint, text, bigint, text, bigint)
RETURNS SETOF accel_tpch_q19_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ19'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q20 CASCADE;
CREATE FUNCTION accel_tpch_q20(date, text, text)
RETURNS TABLE (
  s_name text,
  s_address text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ20'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q21 CASCADE;
CREATE FUNCTION accel_tpch_q21(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ21'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q22 CASCADE;
CREATE FUNCTION accel_tpch_q22(text, text, text, text, text, text, text)
RETURNS TABLE (
  cntrycode text,
  numcust bigint,
  totacctbal numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ22'
LANGUAGE C;







-- Stateful auxiliary kernels (shared AuxQueryModule path)
DROP FUNCTION IF EXISTS accel_tpch_q1_aux CASCADE;
CREATE FUNCTION accel_tpch_q1_aux(integer)
RETURNS TABLE (
  l_returnflag text,
  l_linestatus text,
  sum_qty numeric,
  sum_base_price numeric,
  sum_disc_price numeric,
  sum_charge numeric,
  avg_qty double precision,
  avg_price double precision,
  avg_disc double precision,
  count_order bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ1'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q3_aux CASCADE;
CREATE FUNCTION accel_tpch_q3_aux(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ3'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q6_aux CASCADE;
CREATE FUNCTION accel_tpch_q6_aux(date, double precision, double precision)
RETURNS SETOF accel_tpch_q6_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ6'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q10_aux CASCADE;
CREATE FUNCTION accel_tpch_q10_aux(date)
RETURNS TABLE (
  c_custkey bigint,
  c_name text,
  revenue numeric,
  c_acctbal numeric,
  n_name text,
  c_address text,
  c_phone text,
  c_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ10'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q12_aux CASCADE;
CREATE FUNCTION accel_tpch_q12_aux(text, text, date)
RETURNS TABLE (
  l_shipmode text,
  high_line_count bigint,
  low_line_count bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ12'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q14_aux CASCADE;
CREATE FUNCTION accel_tpch_q14_aux(date)
RETURNS SETOF accel_tpch_q14_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ14'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q9_aux CASCADE;
CREATE FUNCTION accel_tpch_q9_aux(text)
RETURNS TABLE (
  nation text,
  o_year bigint,
  sum_profit numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ9'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q11_aux CASCADE;
CREATE FUNCTION accel_tpch_q11_aux(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ11'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q17_aux CASCADE;
CREATE FUNCTION accel_tpch_q17_aux(text, text)
RETURNS SETOF accel_tpch_q17_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ17'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q18_aux CASCADE;
CREATE FUNCTION accel_tpch_q18_aux(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ18'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q21_aux CASCADE;
CREATE FUNCTION accel_tpch_q21_aux(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ21'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q22_aux CASCADE;
CREATE FUNCTION accel_tpch_q22_aux(text, text, text, text, text, text, text)
RETURNS TABLE (
  cntrycode text,
  numcust bigint,
  totacctbal numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ22'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q2_aux CASCADE;
CREATE FUNCTION accel_tpch_q2_aux(integer, text, text)
RETURNS TABLE (
  s_acctbal numeric,
  s_name text,
  n_name text,
  p_partkey bigint,
  p_mfgr text,
  s_address text,
  s_phone text,
  s_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ2'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q4_aux CASCADE;
CREATE FUNCTION accel_tpch_q4_aux(date)
RETURNS TABLE (
  o_orderpriority text,
  order_count bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ4'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q5_aux CASCADE;
CREATE FUNCTION accel_tpch_q5_aux(text, date)
RETURNS TABLE (
  n_name text,
  revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ5'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q7_aux CASCADE;
CREATE FUNCTION accel_tpch_q7_aux(text, text)
RETURNS TABLE (
  supp_nation text,
  cust_nation text,
  l_year bigint,
  revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ7'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q8_aux CASCADE;
CREATE FUNCTION accel_tpch_q8_aux(text, text, text)
RETURNS TABLE (
  o_year bigint,
  mkt_share double precision
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ8'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q13_aux CASCADE;
CREATE FUNCTION accel_tpch_q13_aux(text)
RETURNS TABLE (
  c_count bigint,
  custdist bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ13'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q15_aux CASCADE;
CREATE FUNCTION accel_tpch_q15_aux(date)
RETURNS TABLE (
  s_suppkey bigint,
  s_name text,
  s_address text,
  s_phone text,
  total_revenue numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ15'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q16_aux CASCADE;
CREATE FUNCTION accel_tpch_q16_aux(text, text, integer, integer, integer, integer, integer, integer, integer, integer)
RETURNS TABLE (
  p_brand text,
  p_type text,
  p_size integer,
  supplier_cnt bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ16'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q19_aux CASCADE;
CREATE FUNCTION accel_tpch_q19_aux(text, bigint, text, bigint, text, bigint)
RETURNS SETOF accel_tpch_q19_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ19'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q20_aux CASCADE;
CREATE FUNCTION accel_tpch_q20_aux(date, text, text)
RETURNS TABLE (
  s_name text,
  s_address text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ20'
LANGUAGE C;












