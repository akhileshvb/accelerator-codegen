UPDATE orders SET o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer B' ORDER BY c_custkey LIMIT 1) WHERE o_comment='q7 point';

UPDATE lineitem SET l_suppkey=(SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier B' ORDER BY s_suppkey LIMIT 1), l_extendedprice=l_extendedprice+12.34, l_shipdate=DATE '1995-09-15' WHERE l_comment IN ('q7 point 1','q7 point 2');

UPDATE supplier SET s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1) WHERE s_comment='q7 supplier B';

UPDATE customer SET c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1) WHERE c_comment='q7 customer B';
