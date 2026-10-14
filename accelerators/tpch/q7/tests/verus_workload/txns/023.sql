UPDATE customer SET c_nationkey=(c_nationkey+1)%25
WHERE c_custkey IN (
 SELECT DISTINCT c.c_custkey FROM customer c
 JOIN orders o ON o.o_custkey=c.c_custkey
 JOIN lineitem l ON l.l_orderkey=o.o_orderkey
 JOIN supplier s ON s.s_suppkey=l.l_suppkey
 WHERE l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
 AND c.c_comment NOT LIKE 'q7 %' ORDER BY c.c_custkey LIMIT 8);
