CREATE OR REPLACE FUNCTION accel_tpch_q7_mv(nation1 text, nation2 text)
RETURNS TABLE (
  supp_nation character varying,
  cust_nation character varying,
  l_year integer,
  revenue numeric
)
LANGUAGE sql STABLE AS $$
  SELECT m.supp_nation, m.cust_nation, m.l_year, m.revenue
  FROM accel_tpch_q7_matview m
  WHERE (m.supp_nation = nation1 AND m.cust_nation = nation2)
     OR (m.supp_nation = nation2 AND m.cust_nation = nation1)
  ORDER BY m.supp_nation, m.cust_nation, m.l_year
$$;
