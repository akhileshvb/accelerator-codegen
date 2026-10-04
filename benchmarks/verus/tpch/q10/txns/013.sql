UPDATE lineitem SET l_discount = 0.08
WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment BETWEEN 'Q10-05' AND 'Q10-12')
  AND l_returnflag = 'R';
