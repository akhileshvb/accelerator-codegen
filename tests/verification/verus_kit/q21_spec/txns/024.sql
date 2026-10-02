UPDATE orders SET o_orderstatus = CASE WHEN o_orderstatus = 'F' THEN 'O' ELSE 'F' END WHERE o_orderkey = (SELECT o_orderkey FROM orders ORDER BY hash(o_orderkey + 24) LIMIT 1);
