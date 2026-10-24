DELETE FROM partsupp WHERE (ps_partkey, ps_suppkey) IN (
    SELECT DISTINCT ps.ps_partkey, ps.ps_suppkey FROM partsupp ps JOIN part p ON p.p_partkey = ps.ps_partkey
    WHERE p.p_name NOT LIKE 'Verus %' AND p.p_type NOT LIKE 'SMALL BRUSHED%'
      AND ps.ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
    ORDER BY ps.ps_partkey, ps.ps_suppkey LIMIT 24
);
