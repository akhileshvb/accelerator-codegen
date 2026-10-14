UPDATE customer SET c_custkey = (SELECT MAX(c_custkey) + 1 FROM customer)
WHERE c_name = 'Q13 pending 2';
UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 pending 2')
WHERE o_comment = 'Pending 2 order';
DELETE FROM orders WHERE o_orderkey = (
    SELECT MIN(o_orderkey) FROM orders WHERE o_comment = 'Pending 2 order');
