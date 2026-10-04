UPDATE lineitem SET l_shipmode = 'AIR', l_comment = 'q12_mode_point'
WHERE (l_orderkey, l_linenumber) IN (
  SELECT l_orderkey, l_linenumber FROM lineitem JOIN orders ON l_orderkey = o_orderkey
  WHERE l_shipmode = 'MAIL' AND l_shipdate < l_commitdate AND l_commitdate < l_receiptdate
    AND l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1996-01-01'
    AND l_comment NOT LIKE 'q12_%'
  ORDER BY l_orderkey, l_linenumber LIMIT 1);
