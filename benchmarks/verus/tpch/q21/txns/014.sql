INSERT INTO orders (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
VALUES (2100000000, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'new answer order');
INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
VALUES
  (2100000000, 1, (
    SELECT s.s_suppkey
    FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey
    WHERE n.n_name = 'SAUDI ARABIA'
    ORDER BY s.s_suppkey
    LIMIT 1
  ), 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-02', '', '', 'late primary'),
  (2100000000, 2, (
    SELECT s.s_suppkey
    FROM supplier s
    WHERE s.s_suppkey <> (
      SELECT q.s_suppkey
      FROM supplier q JOIN nation n ON n.n_nationkey = q.s_nationkey
      WHERE n.n_name = 'SAUDI ARABIA'
      ORDER BY q.s_suppkey
      LIMIT 1
    )
    ORDER BY s.s_suppkey DESC
    LIMIT 1
  ), 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-01', '', '', 'non-late peer');
