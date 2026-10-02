UPDATE orders SET o_orderstatus = 'F' WHERE o_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderstatus <> 'F' ORDER BY o_orderkey LIMIT 40);
