INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) FROM partsupp) + batch.offset_key,
       s.s_suppkey, 1000 + batch.offset_key * 100, 400.00 + batch.offset_key, 'q11-restock-batch'
FROM supplier s CROSS JOIN range(1, 21) AS batch(offset_key)
WHERE s.s_comment = 'q11-depot-b';
