UPDATE orders SET o_orderstatus = 'F' WHERE o_comment = 'q6-new-order-a';
UPDATE lineitem SET l_shipdate = DATE '1995-01-02', l_linestatus = 'F'
WHERE l_comment = 'q6-new-order-a';
