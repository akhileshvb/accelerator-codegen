-- Ordinary order placement: a finished order with lines from three SAUDI ARABIA suppliers;
-- only line 1 is late, so its supplier gains one waiting order.
INSERT INTO orders
SELECT template.* REPLACE (
  (SELECT MAX(o_orderkey) + 1 FROM orders) AS o_orderkey,
  'F' AS o_orderstatus,
  DATE '1996-01-02' AS o_orderdate,
  'q21:ordinary:order' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template;
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q21:ordinary:order') AS l_orderkey,
  pick.line AS l_linenumber,
  pick.s_suppkey AS l_suppkey,
  DATE '1996-01-03' AS l_shipdate,
  DATE '1996-01-10' AS l_commitdate,
  CASE WHEN pick.line = 1 THEN DATE '1996-01-15' ELSE DATE '1996-01-05' END AS l_receiptdate,
  'q21:ordinary:line' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (
  -- three suppliers, SAUDI ARABIA's first (any others if earlier transactions renamed it)
  SELECT s_suppkey, CAST(ROW_NUMBER() OVER (ORDER BY ord, s_suppkey) AS INTEGER) AS line
  FROM (SELECT s_suppkey, CASE WHEN n_name = 'SAUDI ARABIA' THEN 0 ELSE 1 END AS ord
        FROM supplier JOIN nation ON s_nationkey = n_nationkey
        ORDER BY ord, s_suppkey LIMIT 3) AS three
) AS pick;
