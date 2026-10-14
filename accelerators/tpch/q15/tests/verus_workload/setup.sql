CREATE TABLE test_q15_saved_suppliers AS SELECT * FROM supplier;
CREATE TABLE test_q15_saved_lines AS SELECT * FROM lineitem;
CREATE VIEW test_q15_quarter_revenue AS
SELECT DATE_TRUNC('quarter', l_shipdate)::DATE AS quarter, l_suppkey,
       SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem GROUP BY 1, 2;
CREATE VIEW test_q15_winners AS
SELECT quarter, l_suppkey, revenue FROM test_q15_quarter_revenue
QUALIFY ROW_NUMBER() OVER (PARTITION BY quarter ORDER BY revenue DESC, l_suppkey) = 1;
