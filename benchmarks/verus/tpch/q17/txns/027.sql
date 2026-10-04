UPDATE part SET p_partkey = (SELECT MAX(p_partkey) FROM part) + 1 WHERE p_name = 'q17-fixture-02';
UPDATE lineitem SET l_partkey = (SELECT p_partkey FROM part WHERE p_name = 'q17-fixture-02' LIMIT 1) WHERE l_comment = 'q17-base-02';
UPDATE lineitem SET l_partkey = (SELECT p_partkey FROM part WHERE p_name = 'q17-fixture-03' LIMIT 1), l_extendedprice = l_extendedprice + 35.00 WHERE l_comment = 'q17-base-02' AND l_linenumber = 1;
