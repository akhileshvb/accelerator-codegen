INSERT INTO part
SELECT (SELECT ps_partkey FROM partsupp WHERE ps_comment = 'v16 orphan edge'),
       'Verus orphan catalog', p_mfgr, 'Brand#33', 'LARGE STEEL', 49,
       p_container, p_retailprice, 'v16 orphan materialized'
FROM part ORDER BY p_partkey LIMIT 1;
UPDATE part SET p_type = 'SMALL BRUSHE' WHERE p_name = 'Verus catalog 08';
