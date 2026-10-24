UPDATE orders SET o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 6')
WHERE o_orderkey IN (SELECT o_orderkey FROM orders
                    WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 7')
                      AND o_comment NOT LIKE '%unusual%accounts%'
                    ORDER BY o_orderkey LIMIT 3);
