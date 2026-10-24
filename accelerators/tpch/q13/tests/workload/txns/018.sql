UPDATE orders SET o_comment = 'Account review resolved'
WHERE o_orderkey IN (SELECT o_orderkey FROM orders
                    WHERE o_clerk = 'Clerk#Q13' AND o_comment LIKE '%unusual%accounts%'
                    ORDER BY o_orderkey LIMIT 6);
