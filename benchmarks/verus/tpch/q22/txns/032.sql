INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT c_custkey FROM customer
        WHERE c_comment = 'q22-seed' AND substring(c_phone, 1, 2) = '13' AND c_acctbal = 9500.00
        ORDER BY c_custkey LIMIT 1),
       'O', 99.99, DATE '1996-04-02', o_orderpriority,
       o_clerk, o_shippriority, 'q22-duplicate-order'
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'q22-duplicate-order';
