UPDATE lineitem SET l_discount=CASE WHEN l_discount=0.06 THEN 0.02 ELSE 0.06 END WHERE l_comment='q7 point 2';
