-- Batch status change: five open orders with a late line are finished.
UPDATE orders SET o_orderstatus = 'F'
WHERE o_orderkey IN (
  SELECT o_orderkey FROM orders
  WHERE o_orderstatus = 'O'
    AND EXISTS (SELECT 1 FROM lineitem WHERE l_orderkey = o_orderkey AND l_receiptdate > l_commitdate)
  ORDER BY o_orderkey LIMIT 5);
