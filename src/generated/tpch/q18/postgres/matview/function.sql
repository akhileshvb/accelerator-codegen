CREATE OR REPLACE FUNCTION accel_tpch_q18_mv(quantity_threshold numeric)
RETURNS TABLE (
  c_name character varying,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  sum_qty numeric
)
LANGUAGE sql STABLE AS $$
  SELECT m.c_name, m.c_custkey, m.o_orderkey, m.o_orderdate, m.o_totalprice, m.sum_qty
  FROM accel_tpch_q18_matview m
  WHERE m.quantity_threshold = quantity_threshold
  ORDER BY m.o_totalprice DESC, m.o_orderdate
$$;
