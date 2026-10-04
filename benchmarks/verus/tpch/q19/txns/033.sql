DELETE FROM lineitem WHERE rowid = (
    SELECT max(rowid) FROM lineitem WHERE l_comment = 'q19-line-01' AND l_linenumber = 1
);
UPDATE lineitem SET l_partkey = (SELECT max(p_partkey) + 1 FROM part), l_extendedprice = l_extendedprice + 0.01
WHERE l_comment = 'q19-line-12';
UPDATE part SET p_partkey = (SELECT max(p_partkey) + 1 FROM part) WHERE p_name = 'q19-stock-12';
