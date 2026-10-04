INSERT INTO partsupp
SELECT p_partkey, s_suppkey, 21, 10.00, 'q20-base'
FROM part JOIN supplier ON p_name = 'gainsboro test ' || s_suppkey
WHERE p_comment = 'q20-base' AND s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
