UPDATE orders SET o_orderkey = -2147483648, o_custkey = -2147483648,
                  o_totalprice = 9999999999999.99
WHERE o_comment = 'q18:order:06';
UPDATE lineitem SET l_orderkey = -2147483648, l_quantity = 9999999999999.99
WHERE l_comment = 'q18:line:06';
UPDATE customer SET c_custkey = -2147483648 WHERE c_comment = 'q18:customer:06';
UPDATE orders SET o_custkey = -2147483648
WHERE o_comment IN ('q18:batch:06', 'q18:replacement:06');
UPDATE orders SET o_orderkey = 2147483647, o_totalprice = -9999999999999.99
WHERE o_comment = 'q18:order:07';
UPDATE lineitem SET l_orderkey = 2147483647, l_quantity = -9999999999999.99
WHERE l_comment = 'q18:line:07';
