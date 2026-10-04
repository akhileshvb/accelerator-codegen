UPDATE orders SET o_orderdate = CASE o_comment
  WHEN 'q8-order-01' THEN DATE '1994-12-31'
  WHEN 'q8-order-02' THEN DATE '1995-01-01'
  WHEN 'q8-order-03' THEN DATE '1995-12-31'
  WHEN 'q8-order-04' THEN DATE '1996-01-01'
  WHEN 'q8-order-05' THEN DATE '1996-12-31'
  ELSE DATE '1997-01-01' END
WHERE o_comment LIKE 'q8-order-%';
UPDATE lineitem SET l_extendedprice = 4567.89 WHERE l_comment = 'q8-line-01-1';
