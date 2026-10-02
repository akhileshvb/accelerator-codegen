UPDATE orders SET o_orderstatus = 'F' WHERE o_orderkey = (SELECT o_orderkey FROM orders ORDER BY hash(o_orderkey + 38) LIMIT 1);
UPDATE lineitem SET l_suppkey = l_suppkey % 10 + 1 WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 39) LIMIT 1);
