UPDATE orders SET o_orderdate=DATE '1995-06-15'
WHERE o_orderkey=(SELECT min(o_orderkey) FROM orders WHERE o_comment='q5 fixture order');
