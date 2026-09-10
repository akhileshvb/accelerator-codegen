-- Register TPC-H Q3 accelerator SRF against accelerator_codegen_pg.so
DROP FUNCTION IF EXISTS accel_tpch_q3(text, date);
CREATE FUNCTION accel_tpch_q3(market_segment text, cutoff_date date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority integer
)
AS 'accelerator_codegen_pg', 'accel_tpch_q3'
LANGUAGE C STABLE PARALLEL SAFE;
