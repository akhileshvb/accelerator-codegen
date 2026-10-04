INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT max(c_custkey) + 1 FROM customer),
       'O', 8000.00, DATE '1996-03-02', o_orderpriority,
       o_clerk, o_shippriority, 'q22-commercial-order'
FROM orders ORDER BY o_orderkey LIMIT 1;

INSERT INTO customer
SELECT (SELECT o_custkey FROM orders WHERE o_comment = 'q22-commercial-order'),
       'Q22 Commercial Account', c_address, c_nationkey, '13-555-0122',
       50000.00, c_mktsegment, 'q22-commercial'
FROM customer ORDER BY c_custkey LIMIT 1;
