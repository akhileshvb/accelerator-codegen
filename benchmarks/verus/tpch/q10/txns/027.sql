INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q10 Testland', n_regionkey, 'Restored sales territory'
FROM nation ORDER BY n_nationkey LIMIT 1;
UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'Q10 Testland')
WHERE c_name BETWEEN 'Q10-01' AND 'Q10-24';
