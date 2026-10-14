UPDATE lineitem SET l_extendedprice = 9999999999999.99, l_discount = 0.99
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-24') AND l_returnflag = 'R';
UPDATE customer SET c_acctbal = -9999999999999.99 WHERE c_name = 'Q10-24';
