UPDATE partsupp SET ps_availqty = ps_availqty + 25
WHERE (ps_partkey, ps_suppkey) IN (
    SELECT ps.ps_partkey, ps.ps_suppkey FROM partsupp ps
    JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
    JOIN nation n ON n.n_nationkey = s.s_nationkey
    WHERE n.n_name = 'GERMANY'
    ORDER BY ps.ps_supplycost * ps.ps_availqty DESC, ps.ps_partkey, ps.ps_suppkey LIMIT 1
);
