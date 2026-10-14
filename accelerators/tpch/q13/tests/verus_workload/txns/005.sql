UPDATE orders SET o_comment = 'unusual corrected accounts'
WHERE o_orderkey = (SELECT MAX(o_orderkey) FROM orders
                   WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 1')
                     AND o_comment NOT LIKE '%unusual%accounts%');
