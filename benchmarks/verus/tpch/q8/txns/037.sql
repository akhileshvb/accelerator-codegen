UPDATE orders SET o_orderdate = DATE '1995-01-01' WHERE o_comment = 'q8-order-01';
UPDATE orders SET o_orderdate = DATE '1996-12-31' WHERE o_comment = 'q8-order-06';
UPDATE lineitem SET l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-02') WHERE l_comment = 'q8-line-03-1';
UPDATE lineitem SET l_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q8-supplier-b') WHERE l_comment = 'q8-line-04-1';
UPDATE lineitem SET l_partkey = (SELECT p_partkey FROM part WHERE p_comment = 'q8-part-b') WHERE l_comment = 'q8-line-05-1';
