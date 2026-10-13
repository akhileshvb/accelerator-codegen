CREATE OR REPLACE FUNCTION accel_tpch_q12_mv(shipmode1 text, shipmode2 text, start_date date)
RETURNS TABLE (
  l_shipmode character varying,
  high_line_count bigint,
  low_line_count bigint
)
LANGUAGE sql STABLE AS $$
  SELECT
    m.l_shipmode,
    SUM(CASE WHEN m.is_high = 1 THEN m.line_count ELSE 0 END),
    SUM(CASE WHEN m.is_high = 0 THEN m.line_count ELSE 0 END)
  FROM accel_tpch_q12_matview m
  WHERE m.l_shipmode IN (shipmode1, shipmode2)
    AND m.l_receiptdate >= start_date
    AND m.l_receiptdate < start_date + INTERVAL '1' year
  GROUP BY m.l_shipmode
  ORDER BY m.l_shipmode
$$;
