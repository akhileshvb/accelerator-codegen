UPDATE supplier SET s_nationkey=(s_nationkey+1)%25
WHERE s_suppkey IN (
 SELECT DISTINCT s.s_suppkey FROM supplier s
 JOIN lineitem l ON l.l_suppkey=s.s_suppkey
 JOIN orders o ON o.o_orderkey=l.l_orderkey
 JOIN customer c ON c.c_custkey=o.o_custkey
 WHERE l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
 AND s.s_comment NOT LIKE 'q7 %' ORDER BY s.s_suppkey LIMIT 8);
