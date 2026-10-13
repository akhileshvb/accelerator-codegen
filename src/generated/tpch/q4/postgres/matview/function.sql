CREATE OR REPLACE FUNCTION accel_tpch_q4_mv(start_date date)
RETURNS TABLE (o_orderpriority character varying, order_count bigint)
LANGUAGE sql STABLE AS $$
  SELECT m.o_orderpriority, SUM(m.order_count)::bigint
  FROM accel_tpch_q4_matview m
  WHERE m.o_orderdate >= start_date
    AND m.o_orderdate < start_date + INTERVAL '3' month
  GROUP BY m.o_orderpriority
  ORDER BY m.o_orderpriority
$$;
