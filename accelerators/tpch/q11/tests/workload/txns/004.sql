INSERT INTO partsupp
SELECT max(ps_partkey) + 1,
       (SELECT s_suppkey FROM supplier WHERE s_comment = 'q11-depot-a'),
       2000, 84.25, 'q11-new-stock-1' FROM partsupp;
