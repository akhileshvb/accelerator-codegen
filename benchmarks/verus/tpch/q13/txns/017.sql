DELETE FROM orders
WHERE o_orderkey IN (SELECT o_orderkey FROM orders
                    WHERE o_clerk <> 'Clerk#Q13' AND o_comment NOT LIKE '%unusual%accounts%'
                    ORDER BY o_orderkey LIMIT 15);
