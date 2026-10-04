UPDATE lineitem
SET l_linestatus = 'F', l_shipdate = DATE '1998-08-03'
WHERE l_comment = 'q1-new-bulk';

UPDATE orders SET o_orderstatus = 'F'
WHERE o_comment = 'q1-new-bulk';
