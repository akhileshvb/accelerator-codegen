UPDATE orders SET o_orderdate = DATE '1993-10-01' WHERE o_comment BETWEEN 'Q10-01' AND 'Q10-24';
UPDATE lineitem SET l_returnflag = CASE WHEN l_linenumber < 3 THEN 'R' ELSE 'N' END,
       l_extendedprice = 100000.00, l_discount = 0.00
WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment BETWEEN 'Q10-01' AND 'Q10-24');
