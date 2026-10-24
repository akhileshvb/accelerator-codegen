INSERT INTO nation (n_nationkey, n_name, n_regionkey, n_comment)
SELECT s.s_nationkey, 'NO SUCH NATION', 0, 'namespace missing from initial snapshot'
FROM supplier s
WHERE s.s_name = 'namespace pending supplier'
LIMIT 1;
