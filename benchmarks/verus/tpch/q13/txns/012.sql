UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 1')
WHERE o_orderkey = (SELECT MIN(o_orderkey) FROM orders
                   WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 3')
                     AND o_comment NOT LIKE '%unusual%accounts%');
