UPDATE part SET p_brand = 'Brand#45'
WHERE p_partkey = (
    SELECT p_partkey FROM part JOIN lineitem ON p_partkey = l_partkey
    WHERE p_brand = 'Brand#44' AND p_container = 'SM BOX' AND p_size BETWEEN 1 AND 5
      AND l_quantity BETWEEN 1 AND 11 AND l_shipmode IN ('AIR','AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON' AND p_comment <> 'q19 fixture'
    ORDER BY p_partkey LIMIT 1
);
UPDATE lineitem SET l_quantity = l_quantity + 0.01
WHERE l_comment IN ('q19-line-03','q19-line-08','q19-line-10');
