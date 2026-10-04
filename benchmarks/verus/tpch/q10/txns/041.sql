UPDATE lineitem SET l_discount = 1.00
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-01') AND l_returnflag = 'R';
UPDATE lineitem SET l_extendedprice = 0.01, l_discount = 0.01
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-02') AND l_returnflag = 'R';
UPDATE lineitem SET l_discount = 1.01
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-03') AND l_returnflag = 'R';
UPDATE orders SET o_orderdate = DATE '1993-12-31'
WHERE o_comment IN ('Q10-01', 'Q10-02', 'Q10-03');
