INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT max(l_linenumber) + 1 FROM lineitem WHERE l_orderkey = template.l_orderkey) AS l_linenumber,
  'MAIL' AS l_shipmode, 'q12_extra_line' AS l_comment)
FROM (
  SELECT lineitem.* FROM lineitem JOIN orders ON l_orderkey = o_orderkey
  WHERE l_shipdate < l_commitdate AND l_commitdate < l_receiptdate
    AND l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1996-01-01'
    AND l_comment NOT LIKE 'q12_%'
  ORDER BY l_orderkey, l_linenumber LIMIT 1) template;
