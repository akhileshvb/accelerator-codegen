UPDATE lineitem SET l_extendedprice=CASE l_linenumber WHEN 1 THEN 9999999999999.99 ELSE -999999999999.99 END, l_discount=0.00
WHERE l_comment='q5 fixture line' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment='q5 fixture supplier' AND s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='INDIA'));
UPDATE lineitem SET l_extendedprice=0.01, l_discount=9999999999999.99
WHERE l_comment='q5 replacement order line' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment='q5 fixture supplier' AND s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='INDIA'));
