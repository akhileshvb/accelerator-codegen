-- point_delete / lineitem on real data: delete one joined in-window line (the largest by value), the
-- shape a cancellation takes. Both sums must shrink by exactly that row's contribution.
DELETE FROM lineitem
WHERE (l_orderkey, l_linenumber) IN (
    SELECT l.l_orderkey, l.l_linenumber FROM lineitem l JOIN part p ON p.p_partkey = l.l_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
     ORDER BY l.l_extendedprice * (1 - l.l_discount) DESC, l.l_orderkey, l.l_linenumber LIMIT 1);
