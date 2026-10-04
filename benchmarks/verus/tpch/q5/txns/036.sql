UPDATE orders SET o_orderkey=-o_orderkey WHERE o_orderkey=(SELECT min(o_orderkey) FROM orders WHERE o_comment='q5 fixture order' AND o_custkey=(SELECT c_custkey FROM customer JOIN nation ON c_nationkey=n_nationkey WHERE c_comment='q5 fixture customer' AND n_name='INDIA'));
UPDATE customer SET c_custkey=-c_custkey WHERE c_custkey=(SELECT c_custkey FROM customer JOIN nation ON c_nationkey=n_nationkey WHERE c_comment='q5 fixture customer' AND n_name='INDIA');
UPDATE supplier SET s_suppkey=-s_suppkey WHERE s_suppkey=(SELECT s_suppkey FROM supplier JOIN nation ON s_nationkey=n_nationkey WHERE s_comment='q5 fixture supplier' AND n_name='INDIA');
UPDATE nation SET n_nationkey=-n_nationkey WHERE n_name='INDIA';
UPDATE region SET r_regionkey=-r_regionkey WHERE r_name='ASIA';
