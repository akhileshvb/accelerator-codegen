UPDATE lineitem SET l_extendedprice = l_extendedprice + 17.29
WHERE rowid = (
    SELECT l.rowid FROM lineitem AS l JOIN part ON p_partkey = l_partkey
    WHERE p_brand = 'Brand#44' AND p_container = 'SM BOX' AND p_size BETWEEN 1 AND 5
      AND l_quantity BETWEEN 1 AND 11 AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON' AND p_comment <> 'q19 fixture'
    ORDER BY l_orderkey, l_linenumber LIMIT 1
);
