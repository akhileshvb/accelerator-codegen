UPDATE lineitem SET l_extendedprice = round(l_extendedprice * 1.05, 2)
WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment BETWEEN 'Q10-01' AND 'Q10-24')
  AND l_returnflag = 'R';
