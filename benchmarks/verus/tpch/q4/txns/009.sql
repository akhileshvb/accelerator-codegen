UPDATE orders SET o_orderdate = o_orderdate - INTERVAL '3 months'
WHERE o_comment LIKE 'q4:batch:%';
