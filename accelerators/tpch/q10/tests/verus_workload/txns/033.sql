INSERT INTO customer SELECT * FROM customer WHERE c_name = 'Q10-01';
INSERT INTO nation SELECT * FROM nation WHERE n_name = 'Q10 Federation';
INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'Q10-01';
INSERT INTO lineitem SELECT * FROM lineitem
WHERE l_orderkey = (SELECT min(o_orderkey) FROM orders WHERE o_comment = 'Q10-01') AND l_linenumber = 1;
