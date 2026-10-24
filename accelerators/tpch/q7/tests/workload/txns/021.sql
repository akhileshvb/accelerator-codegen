UPDATE lineitem SET l_extendedprice=l_extendedprice+0.01, l_discount=CASE WHEN l_discount<0.09 THEN l_discount+0.01 ELSE 0.02 END WHERE (l_orderkey,l_linenumber) IN (SELECT l.l_orderkey, l.l_linenumber FROM lineitem l
JOIN orders o ON o.o_orderkey=l.l_orderkey
JOIN customer c ON c.c_custkey=o.o_custkey
JOIN supplier s ON s.s_suppkey=l.l_suppkey
JOIN nation ns ON ns.n_nationkey=s.s_nationkey
JOIN nation nc ON nc.n_nationkey=c.c_nationkey
WHERE l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
AND l.l_comment NOT LIKE 'q7 %'
ORDER BY l.l_orderkey,l.l_linenumber LIMIT 64);
