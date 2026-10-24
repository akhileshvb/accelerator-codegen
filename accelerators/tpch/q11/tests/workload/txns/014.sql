UPDATE partsupp SET ps_supplycost = ps_supplycost * 1.03
WHERE ps_suppkey IN (
    SELECT s.s_suppkey FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey
    WHERE n.n_name IN ('UNITED STATES', 'KENYA', 'ROMANIA')
);
