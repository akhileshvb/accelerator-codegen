DELETE FROM partsupp WHERE ps_comment = 'q11-restock-batch'
AND ps_partkey IN (SELECT ps_partkey FROM partsupp WHERE ps_comment = 'q11-restock-batch' ORDER BY ps_partkey LIMIT 10);
