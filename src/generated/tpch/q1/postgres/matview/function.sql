CREATE OR REPLACE FUNCTION accel_tpch_q1_mv(day_offset integer)
RETURNS TABLE (
  l_returnflag character varying,
  l_linestatus character varying,
  sum_qty numeric,
  sum_base_price numeric,
  sum_disc_price numeric,
  sum_charge numeric,
  avg_qty numeric,
  avg_price numeric,
  avg_disc numeric,
  count_order bigint
)
LANGUAGE sql
STABLE
AS $$
  SELECT
    m.l_returnflag,
    m.l_linestatus,
    SUM(m.sum_qty),
    SUM(m.sum_base_price),
    SUM(m.sum_disc_price),
    SUM(m.sum_charge),
    SUM(m.sum_qty) / SUM(m.count_order),
    SUM(m.sum_base_price) / SUM(m.count_order),
    SUM(m.sum_discount) / SUM(m.count_order),
    SUM(m.count_order)
  FROM accel_tpch_q1_matview m
  WHERE m.l_shipdate <= DATE '1998-12-01' - (day_offset || ' days')::interval
  GROUP BY m.l_returnflag, m.l_linestatus
  ORDER BY m.l_returnflag, m.l_linestatus
$$;
