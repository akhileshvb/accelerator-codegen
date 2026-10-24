DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 fixture order' AND o_orderdate=DATE '1995-07-01' ORDER BY o_orderkey LIMIT 7));
DELETE FROM orders WHERE o_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 fixture order' AND o_orderdate=DATE '1995-07-01' ORDER BY o_orderkey LIMIT 7);
