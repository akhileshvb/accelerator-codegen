DELETE FROM lineitem
WHERE l_orderkey IN (
  SELECT o_orderkey FROM orders WHERE o_clerk = 'Clerk#000014014'
);

DELETE FROM orders WHERE o_clerk = 'Clerk#000014014';
