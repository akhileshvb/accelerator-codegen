UPDATE lineitem SET l_extendedprice=CASE l_linenumber WHEN 1 THEN 1200.00 ELSE 2450.00 END, l_discount=CASE l_linenumber WHEN 1 THEN 0.03 ELSE 0.07 END WHERE l_comment='q5 fixture line';
