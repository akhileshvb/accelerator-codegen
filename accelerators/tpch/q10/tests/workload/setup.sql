INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q10 Testland', n_regionkey, 'Q10 fixture nation'
FROM nation ORDER BY n_nationkey LIMIT 1;

INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer) + fixture_index,
       'Q10-' || lpad(CAST(fixture_index AS VARCHAR), 2, '0'),
       'Return customer ' || CAST(fixture_index AS VARCHAR),
       (SELECT n_nationkey FROM nation WHERE n_name = 'Q10 Testland'),
       '10-100-100-1000', 1000.00 + fixture_index, 'BUILDING', 'Q10 fixture customer'
FROM range(1, 25) AS fixtures(fixture_index);

INSERT INTO orders
SELECT (SELECT max(o_orderkey) FROM orders) + row_number() OVER (ORDER BY c_custkey),
       c_custkey, 'F', 300000.00, DATE '1993-10-01', '2-HIGH', 'Clerk#000000001', 0, c_name
FROM customer WHERE c_name LIKE 'Q10-%';

INSERT INTO lineitem
SELECT o_orderkey, template.l_partkey, template.l_suppkey, fixture_line,
       10.00, 120000.00 + (24 - CAST(o_comment[5:6] AS INTEGER)) * 1000.00,
       0.05, 0.02, CASE WHEN fixture_line < 3 THEN 'R' ELSE 'N' END, 'F',
       DATE '1993-10-15', DATE '1993-10-12', DATE '1993-10-20', 'DELIVER IN PERSON', 'AIR', o_comment
FROM orders CROSS JOIN range(1, 4) AS fixture_lines(fixture_line)
CROSS JOIN (SELECT l_partkey, l_suppkey FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
WHERE o_comment LIKE 'Q10-%';
