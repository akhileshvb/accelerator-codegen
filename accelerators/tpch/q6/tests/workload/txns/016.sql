UPDATE lineitem SET l_extendedprice = l_extendedprice + 17.23
WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'
  AND l_discount BETWEEN 0.02 AND 0.09 AND l_quantity < 25 AND l_comment NOT LIKE 'q6-%';
