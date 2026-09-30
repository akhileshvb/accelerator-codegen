-- Q21 accelerator backed by the Verus-verified kernel (src/verus/q21).
DROP FUNCTION IF EXISTS accel_tpch_q21_verus CASCADE;
CREATE FUNCTION accel_tpch_q21_verus(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ21Verus'
LANGUAGE C STRICT;  -- NULL :nation matches no nation: empty result
