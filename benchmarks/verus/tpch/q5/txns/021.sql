UPDATE lineitem SET l_extendedprice=round(l_extendedprice*1.10,2), l_discount=0.02 WHERE l_comment='q5 fixture line';
