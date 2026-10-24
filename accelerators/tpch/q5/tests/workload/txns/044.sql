UPDATE region SET r_name='Q5 EDGE' WHERE r_comment='q5 edge region';
DELETE FROM nation WHERE n_comment='q5 tied nation';
INSERT INTO nation SELECT seed.* REPLACE (group_name AS n_name, (SELECT r_regionkey FROM region WHERE r_comment='q5 edge region') AS n_regionkey, 'q5 tied nation' AS n_comment) FROM nation seed CROSS JOIN (VALUES ('Q5 EDGE A'), ('Q5 EDGE B')) names(group_name) WHERE n_name='INDIA';
UPDATE lineitem SET l_extendedprice=1234.56, l_discount=0.09 WHERE l_comment='q5 fixture line' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment='q5 fixture supplier' AND s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='INDIA'));
