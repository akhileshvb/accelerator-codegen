-- RF2-shaped bulk delete: the 200 bulk orders above with their lines, and 200 more of the oldest orders with theirs
DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'BULK-Q3-022');
DELETE FROM orders WHERE o_comment = 'BULK-Q3-022';
DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT 200);
DELETE FROM orders WHERE o_orderkey IN (SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT 200);
