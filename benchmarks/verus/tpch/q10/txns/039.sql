UPDATE lineitem SET l_orderkey = (SELECT max(o_orderkey) + 100 FROM orders)
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-03');
UPDATE orders SET o_orderkey = (SELECT DISTINCT l_orderkey FROM lineitem WHERE l_comment = 'Q10-03')
WHERE o_comment = 'Q10-03';
UPDATE orders SET o_custkey = (SELECT max(c_custkey) + 100 FROM customer)
WHERE o_comment = 'Q10-03';
UPDATE customer SET c_custkey = (SELECT o_custkey FROM orders WHERE o_comment = 'Q10-03')
WHERE c_name = 'Q10-03';
UPDATE customer SET c_nationkey = (SELECT max(n_nationkey) + 100 FROM nation)
WHERE c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'q10 Federation ');
UPDATE nation SET n_nationkey = (SELECT c_nationkey FROM customer WHERE c_name = 'Q10-03')
WHERE n_name = 'q10 Federation ';
