INSERT INTO part SELECT p_partkey, p_name, p_mfgr, p_brand, 'STANDARD STEEL', p_size, p_container, p_retailprice, p_comment FROM part WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
