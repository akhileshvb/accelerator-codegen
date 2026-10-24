INSERT INTO orders
SELECT (SELECT DISTINCT l_orderkey FROM lineitem WHERE l_comment = 'Q10-01'),
       (SELECT c_custkey FROM customer WHERE c_name = 'Q10-01'),
       o_orderstatus, o_totalprice, DATE '1993-10-01', o_orderpriority, o_clerk, o_shippriority, 'Q10-01'
FROM orders WHERE o_comment = 'Q10-02';
