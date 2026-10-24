UPDATE lineitem SET l_shipdate=CASE WHEN EXTRACT(year FROM l_shipdate)=1995 THEN DATE '1996-06-15' ELSE DATE '1995-06-15' END WHERE l_comment='q7 point 1';
