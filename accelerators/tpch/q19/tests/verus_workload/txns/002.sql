UPDATE lineitem SET l_discount = 0.03
WHERE l_partkey IN (
    SELECT p_partkey FROM part WHERE p_comment <> 'q19 fixture'
      AND ((p_size BETWEEN 1 AND 5 AND p_container IN ('SM CASE','SM BOX','SM PACK','SM PKG'))
        OR (p_size BETWEEN 1 AND 10 AND p_container IN ('MED BAG','MED BOX','MED PACK','MED PKG'))
        OR (p_size BETWEEN 1 AND 15 AND p_container IN ('LG CASE','LG BOX','LG PACK','LG PKG')))
) AND l_shipmode IN ('AIR','AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON';
