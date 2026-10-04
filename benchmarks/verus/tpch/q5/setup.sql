INSERT INTO customer
SELECT seed.* REPLACE (
    ((SELECT max(c_custkey) FROM customer) + row_number() OVER (ORDER BY n_nationkey))::INTEGER AS c_custkey,
    'Q5 customer:' || n_nationkey AS c_name,
    n_nationkey AS c_nationkey,
    'q5 fixture customer' AS c_comment
)
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) seed CROSS JOIN nation;

INSERT INTO supplier
SELECT seed.* REPLACE (
    ((SELECT max(s_suppkey) FROM supplier) + row_number() OVER (ORDER BY n_nationkey))::INTEGER AS s_suppkey,
    'Q5 supplier:' || n_nationkey AS s_name,
    n_nationkey AS s_nationkey,
    'q5 fixture supplier' AS s_comment
)
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) seed CROSS JOIN nation;

INSERT INTO orders
SELECT seed.* REPLACE (
    ((SELECT max(o_orderkey) FROM orders) + row_number() OVER (ORDER BY c_custkey, order_date))::INTEGER AS o_orderkey,
    c_custkey AS o_custkey,
    order_date AS o_orderdate,
    'q5 fixture order' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) seed
CROSS JOIN customer
CROSS JOIN (VALUES (DATE '1994-01-01'), (DATE '1994-12-31'),
                   (DATE '1995-01-01'), (DATE '1995-07-01')) dates(order_date)
WHERE c_comment = 'q5 fixture customer';

INSERT INTO lineitem
SELECT seed.* REPLACE (
    o_orderkey AS l_orderkey, s_suppkey AS l_suppkey,
    line_number AS l_linenumber, price AS l_extendedprice, discount AS l_discount,
    'q5 fixture line' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) seed
JOIN orders ON o_comment = 'q5 fixture order'
JOIN customer ON c_custkey = o_custkey AND c_comment = 'q5 fixture customer'
JOIN supplier ON s_nationkey = c_nationkey AND s_comment = 'q5 fixture supplier'
CROSS JOIN (VALUES (1, 1200.00, 0.03), (2, 2450.00, 0.07)) amounts(line_number, price, discount);
