UPDATE orders SET o_totalprice = o_totalprice + 100.00
WHERE o_comment LIKE 'q18:batch:%';
