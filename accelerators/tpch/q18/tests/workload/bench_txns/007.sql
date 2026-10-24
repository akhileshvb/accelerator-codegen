UPDATE orders SET o_totalprice = o_totalprice - 12.34, o_orderdate = o_orderdate - 1
WHERE o_comment = 'q18:order:01';
