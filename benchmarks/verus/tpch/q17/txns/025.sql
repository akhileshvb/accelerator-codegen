INSERT INTO part SELECT * FROM part WHERE p_name = 'q17-fixture-01';
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_partkey = (SELECT p_partkey FROM part WHERE p_name = 'q17-fixture-01' LIMIT 1) AND l_linenumber = 1 LIMIT 1;
