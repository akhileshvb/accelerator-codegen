UPDATE lineitem
SET l_extendedprice = l_extendedprice + 17.23,
    l_discount = 0.06,
    l_partkey = CASE WHEN l_linenumber % 2 = 0
        THEN (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo')
        ELSE (SELECT p_partkey FROM part WHERE p_comment = 'v14-regular') END,
    l_shipdate = DATE '1995-09-01' + CAST(l_linenumber % 30 AS INTEGER)
WHERE l_comment = 'v14-batch-line';
