UPDATE lineitem SET l_quantity = l_quantity - 1
WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name IN ('q19-stock-01','q19-stock-02'));
