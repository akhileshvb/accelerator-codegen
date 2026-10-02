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

CREATE FUNCTION accel_aux_capture_transition()
RETURNS trigger
AS 'MODULE_PATHNAME', 'AcceleratorCodegenCaptureTransition'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_observer_mutation_count()
RETURNS bigint
AS 'MODULE_PATHNAME', 'accel_postgres_get_observer_mutation_count'
LANGUAGE C STRICT;

CREATE FUNCTION accel_postgres_reset_observer()
RETURNS void
AS 'MODULE_PATHNAME', 'accel_postgres_reset_observer'
LANGUAGE C;

CREATE FUNCTION accel_postgres_set_force_maintenance_action(text)
RETURNS void
AS 'MODULE_PATHNAME', 'accel_postgres_set_force_maintenance_action'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_last_maintenance_decision()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_last_maintenance_decision'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_execute_counts()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_execute_counts'
LANGUAGE C STRICT;

CREATE FUNCTION accel_postgres_get_last_maintenance_action()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_last_maintenance_action'
LANGUAGE C STRICT;

-- Base codegen kernels (stateless native SPI scans)
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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ1'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q3(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ3'
LANGUAGE C;

CREATE TYPE accel_tpch_q6_row AS (revenue numeric);

CREATE FUNCTION accel_tpch_q6(date, double precision, double precision)
RETURNS SETOF accel_tpch_q6_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ6'
LANGUAGE C;

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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ10'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q12(text, text, date)
RETURNS TABLE (
  l_shipmode text,
  high_line_count bigint,
  low_line_count bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ12'
LANGUAGE C;

CREATE TYPE accel_tpch_q14_row AS (promo_revenue double precision);

CREATE FUNCTION accel_tpch_q14(date)
RETURNS SETOF accel_tpch_q14_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ14'
LANGUAGE C;

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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ2'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q4(date)
RETURNS TABLE (
  o_orderpriority text,
  order_count bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ4'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q5(text, date)
RETURNS TABLE (
  n_name text,
  revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ5'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q7(text, text)
RETURNS TABLE (
  supp_nation text,
  cust_nation text,
  l_year bigint,
  revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ7'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q8(text, text, text)
RETURNS TABLE (
  o_year bigint,
  mkt_share double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ8'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q9(text)
RETURNS TABLE (
  nation text,
  o_year bigint,
  sum_profit numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ9'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q11(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ11'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q13(text)
RETURNS TABLE (
  c_count bigint,
  custdist bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ13'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q15(date)
RETURNS TABLE (
  s_suppkey bigint,
  s_name text,
  s_address text,
  s_phone text,
  total_revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ15'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q16(text, text, integer, integer, integer, integer, integer, integer, integer, integer)
RETURNS TABLE (
  p_brand text,
  p_type text,
  p_size integer,
  supplier_cnt bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ16'
LANGUAGE C;

CREATE TYPE accel_tpch_q17_row AS (avg_yearly double precision);

CREATE FUNCTION accel_tpch_q17(text, text)
RETURNS SETOF accel_tpch_q17_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ17'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q18(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ18'
LANGUAGE C;

CREATE TYPE accel_tpch_q19_row AS (revenue numeric);

CREATE FUNCTION accel_tpch_q19(text, bigint, text, bigint, text, bigint)
RETURNS SETOF accel_tpch_q19_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ19'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q20(date, text, text)
RETURNS TABLE (
  s_name text,
  s_address text
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ20'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q21(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ21'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q22(text, text, text, text, text, text, text)
RETURNS TABLE (
  cntrycode text,
  numcust bigint,
  totacctbal numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenBaseTpchQ22'
LANGUAGE C;







-- Stateful auxiliary kernels (shared AuxQueryModule path)
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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ1'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q3_aux(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ3'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q6_aux(date, double precision, double precision)
RETURNS SETOF accel_tpch_q6_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ6'
LANGUAGE C;

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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ10'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q12_aux(text, text, date)
RETURNS TABLE (
  l_shipmode text,
  high_line_count bigint,
  low_line_count bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ12'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q14_aux(date)
RETURNS SETOF accel_tpch_q14_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ14'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q9_aux(text)
RETURNS TABLE (
  nation text,
  o_year bigint,
  sum_profit numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ9'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q11_aux(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ11'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q17_aux(text, text)
RETURNS SETOF accel_tpch_q17_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ17'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q18_aux(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ18'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q21_aux(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ21'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q22_aux(text, text, text, text, text, text, text)
RETURNS TABLE (
  cntrycode text,
  numcust bigint,
  totacctbal numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ22'
LANGUAGE C;

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
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ2'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q4_aux(date)
RETURNS TABLE (
  o_orderpriority text,
  order_count bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ4'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q5_aux(text, date)
RETURNS TABLE (
  n_name text,
  revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ5'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q7_aux(text, text)
RETURNS TABLE (
  supp_nation text,
  cust_nation text,
  l_year bigint,
  revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ7'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q8_aux(text, text, text)
RETURNS TABLE (
  o_year bigint,
  mkt_share double precision
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ8'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q13_aux(text)
RETURNS TABLE (
  c_count bigint,
  custdist bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ13'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q15_aux(date)
RETURNS TABLE (
  s_suppkey bigint,
  s_name text,
  s_address text,
  s_phone text,
  total_revenue numeric
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ15'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q16_aux(text, text, integer, integer, integer, integer, integer, integer, integer, integer)
RETURNS TABLE (
  p_brand text,
  p_type text,
  p_size integer,
  supplier_cnt bigint
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ16'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q19_aux(text, bigint, text, bigint, text, bigint)
RETURNS SETOF accel_tpch_q19_row
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ19'
LANGUAGE C;

CREATE FUNCTION accel_tpch_q20_aux(date, text, text)
RETURNS TABLE (
  s_name text,
  s_address text
)
AS 'MODULE_PATHNAME', 'AcceleratorCodegenAuxTpchQ20'
LANGUAGE C;
