UPDATE lineitem SET l_suppkey=(SELECT max(s_suppkey)+1 FROM supplier) WHERE l_suppkey=(SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1);

UPDATE supplier SET s_suppkey=(SELECT max(s_suppkey)+1 FROM supplier) WHERE s_comment='q7 supplier A';

UPDATE orders SET o_custkey=(SELECT max(c_custkey)+1 FROM customer) WHERE o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1);

UPDATE customer SET c_custkey=(SELECT max(c_custkey)+1 FROM customer) WHERE c_comment='q7 customer A';

UPDATE lineitem SET l_orderkey=(SELECT max(o_orderkey)+1 FROM orders) WHERE l_orderkey=(SELECT o_orderkey FROM orders WHERE o_comment='q7 live' ORDER BY o_orderkey LIMIT 1);

UPDATE orders SET o_orderkey=(SELECT max(o_orderkey)+1 FROM orders), o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer B' ORDER BY c_custkey LIMIT 1) WHERE o_comment='q7 live';

UPDATE supplier SET s_nationkey=(SELECT max(n_nationkey)+1 FROM nation) WHERE s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1);

UPDATE customer SET c_nationkey=(SELECT max(n_nationkey)+1 FROM nation) WHERE c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1);

UPDATE nation SET n_nationkey=(SELECT max(n_nationkey)+1 FROM nation) WHERE n_name='FRANCE';

UPDATE orders SET o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1) WHERE o_comment='q7 live';

UPDATE lineitem SET l_extendedprice=l_extendedprice+0.01 WHERE l_comment='q7 live 1';
