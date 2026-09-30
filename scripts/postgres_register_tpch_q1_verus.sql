-- Stateless TPC-H Q1 aggregated by the Verus-verified kernel (src/verus/q1).
DROP FUNCTION IF EXISTS accel_tpch_q1_verus(integer) CASCADE;
CREATE FUNCTION accel_tpch_q1_verus(integer)
RETURNS TABLE (l_returnflag text, l_linestatus text, sum_qty numeric, sum_base_price numeric,
               sum_disc_price numeric, sum_charge numeric, avg_qty numeric, avg_price numeric,
               avg_disc numeric, count_order bigint)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenTpchQ1Verus'
LANGUAGE C STRICT;
