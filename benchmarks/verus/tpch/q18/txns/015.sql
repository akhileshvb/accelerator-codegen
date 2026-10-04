UPDATE orders SET o_orderdate = o_orderdate + 7
WHERE o_comment LIKE 'q18:batch:%';
