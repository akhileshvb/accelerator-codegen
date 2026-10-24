UPDATE part SET p_comment = 'v14-retire'
WHERE p_partkey IN (
    SELECT DISTINCT l_partkey FROM lineitem
    WHERE l_shipdate >= DATE '1995-03-01' AND l_shipdate < DATE '1995-04-01'
      AND l_comment NOT LIKE 'v14-%'
    ORDER BY l_partkey DESC LIMIT 4
);
DELETE FROM lineitem
WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_comment = 'v14-retire');
DELETE FROM part WHERE p_comment = 'v14-retire';
