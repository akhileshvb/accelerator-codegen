UPDATE orders SET o_totalprice = o_totalprice + 35.75,
                  o_orderdate = o_orderdate + 1
WHERE o_comment = 'q18:new:order';
