UPDATE lineitem SET l_extendedprice = l_extendedprice + 200.12, l_discount = 0.04
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-20') AND l_returnflag = 'R';
UPDATE orders SET o_orderdate = DATE '1993-10-01', o_totalprice = o_totalprice + 400.24
WHERE o_comment = 'Q10-20';
UPDATE customer SET c_name = 'Q10-20', c_address = 'Updated return address', c_acctbal = c_acctbal + 400.24
WHERE c_name = 'Q10-20';
UPDATE nation SET n_name = 'Q10 Federation' WHERE n_name = 'Q10 Testland';
