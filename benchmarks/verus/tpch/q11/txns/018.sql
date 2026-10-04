UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA')
WHERE s_suppkey IN (
    SELECT s.s_suppkey FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey
    WHERE n.n_name = 'UNITED STATES'
      AND EXISTS (SELECT 1 FROM partsupp ps WHERE ps.ps_suppkey = s.s_suppkey AND ps.ps_availqty > 0)
    ORDER BY s.s_suppkey LIMIT 4
);
