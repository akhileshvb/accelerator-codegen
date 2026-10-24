UPDATE nation SET n_regionkey=-n_regionkey WHERE EXISTS (SELECT 1 FROM region WHERE r_regionkey=-n_regionkey AND r_regionkey<0);
UPDATE customer SET c_nationkey=-c_nationkey WHERE EXISTS (SELECT 1 FROM nation WHERE n_nationkey=-c_nationkey AND n_nationkey<0);
UPDATE supplier SET s_nationkey=-s_nationkey WHERE EXISTS (SELECT 1 FROM nation WHERE n_nationkey=-s_nationkey AND n_nationkey<0);
UPDATE orders SET o_custkey=-o_custkey WHERE EXISTS (SELECT 1 FROM customer WHERE c_custkey=-o_custkey AND c_custkey<0);
UPDATE lineitem SET l_orderkey=-l_orderkey WHERE EXISTS (SELECT 1 FROM orders WHERE o_orderkey=-l_orderkey AND o_orderkey<0);
UPDATE lineitem SET l_suppkey=-l_suppkey WHERE EXISTS (SELECT 1 FROM supplier WHERE s_suppkey=-l_suppkey AND s_suppkey<0);
