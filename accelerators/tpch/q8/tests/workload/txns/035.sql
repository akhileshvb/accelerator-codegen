UPDATE lineitem SET l_orderkey = (SELECT max(o_orderkey) + 1 FROM orders)
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-01');
UPDATE orders SET o_orderkey = (SELECT max(o_orderkey) + 1 FROM orders) WHERE o_comment = 'q8-order-01';
UPDATE lineitem SET l_partkey = (SELECT max(p_partkey) + 1 FROM part)
WHERE l_partkey = (SELECT p_partkey FROM part WHERE p_comment = 'q8-part-a');
UPDATE part SET p_partkey = (SELECT max(p_partkey) + 1 FROM part), p_type = 'LARGE STEEL' WHERE p_comment = 'q8-part-a';
UPDATE lineitem SET l_suppkey = (SELECT max(s_suppkey) + 1 FROM supplier)
WHERE l_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q8-supplier-a');
UPDATE supplier SET s_suppkey = (SELECT max(s_suppkey) + 1 FROM supplier) WHERE s_comment = 'q8-supplier-a';
UPDATE orders SET o_custkey = (SELECT max(c_custkey) + 1 FROM customer)
WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_comment = 'q8-customer-a');
UPDATE customer SET c_custkey = (SELECT max(c_custkey) + 1 FROM customer) WHERE c_comment = 'q8-customer-a';
UPDATE supplier SET s_nationkey = (SELECT max(n_nationkey) + 1 FROM nation)
WHERE s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL');
UPDATE customer SET c_nationkey = (SELECT max(n_nationkey) + 1 FROM nation)
WHERE c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL');
UPDATE nation SET n_nationkey = (SELECT max(n_nationkey) + 1 FROM nation) WHERE n_name = 'BRAZIL';
UPDATE nation SET n_regionkey = (SELECT max(r_regionkey) + 1 FROM region)
WHERE n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'AMERICA');
UPDATE region SET r_regionkey = (SELECT max(r_regionkey) + 1 FROM region) WHERE r_name = 'AMERICA';
