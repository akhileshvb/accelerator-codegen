INSERT INTO part SELECT * FROM part WHERE p_name = 'Verus catalog 11';
INSERT INTO partsupp SELECT * FROM partsupp
WHERE ps_partkey = (SELECT min(p_partkey) FROM part WHERE p_name = 'Verus catalog 11');
INSERT INTO part
SELECT p_partkey, 'Verus duplicate 11', p_mfgr, 'Brand#45 ', 'large steel', 49,
       p_container, p_retailprice, 'v16 duplicate group'
FROM part WHERE p_name = 'Verus catalog 11' ORDER BY rowid LIMIT 1;
