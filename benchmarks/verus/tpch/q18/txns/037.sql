UPDATE orders SET o_orderkey = (SELECT MAX(o_orderkey) + 1 FROM orders)
WHERE o_comment = 'q18:order:05';
UPDATE customer SET c_custkey = (SELECT MAX(c_custkey) + 1 FROM customer)
WHERE c_comment = 'q18:customer:05';
