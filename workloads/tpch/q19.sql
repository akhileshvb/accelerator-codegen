-- TPC-H Q19 (catalog text), :brand1 as $1, :quantity1 as $2, :brand2 as $3, :quantity2 as $4, :brand3 as $5, :quantity3 as $6 (upper bound written l_quantity - 10 <= $n, equal to l_quantity <= $n + 10: sql2verus cannot type a parameter used at two types).
SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem, part
WHERE (p_partkey = l_partkey
       AND p_brand = $1
       AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
       AND l_quantity >= $2 AND l_quantity - 10 <= $2
       AND p_size BETWEEN 1 AND 5
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
   OR (p_partkey = l_partkey
       AND p_brand = $3
       AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
       AND l_quantity >= $4 AND l_quantity - 10 <= $4
       AND p_size BETWEEN 1 AND 10
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
   OR (p_partkey = l_partkey
       AND p_brand = $5
       AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
       AND l_quantity >= $6 AND l_quantity - 10 <= $6
       AND p_size BETWEEN 1 AND 15
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
