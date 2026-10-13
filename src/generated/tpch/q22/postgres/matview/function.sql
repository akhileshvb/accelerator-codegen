CREATE OR REPLACE FUNCTION accel_tpch_q22_mv(
  country_code1 text, country_code2 text, country_code3 text, country_code4 text,
  country_code5 text, country_code6 text, country_code7 text
)
RETURNS TABLE (cntrycode text, numcust bigint, totacctbal numeric)
LANGUAGE sql STABLE AS $$
  SELECT m.cntrycode, m.numcust, m.totacctbal
  FROM accel_tpch_q22_matview m
  WHERE m.codeset = 'seed1'
    AND m.cntrycode IN (
      country_code1, country_code2, country_code3, country_code4,
      country_code5, country_code6, country_code7
    )
  ORDER BY m.cntrycode
$$;
