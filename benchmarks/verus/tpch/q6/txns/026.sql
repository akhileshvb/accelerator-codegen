DELETE FROM lineitem WHERE rowid = (
    SELECT rowid FROM lineitem WHERE l_comment = 'q6-boundary-7' ORDER BY rowid LIMIT 1
);
UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01,
    l_orderkey = (SELECT MAX(l_orderkey) + 1 FROM lineitem), l_linenumber = 1
WHERE rowid = (
    SELECT rowid FROM lineitem WHERE l_comment = 'q6-boundary-7' ORDER BY rowid LIMIT 1
);
