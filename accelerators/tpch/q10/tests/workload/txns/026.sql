UPDATE customer SET c_nationkey = (SELECT min(n_nationkey) FROM nation WHERE n_name NOT LIKE 'Q10%')
WHERE c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'Q10 Testland');
DELETE FROM nation WHERE n_name = 'Q10 Testland';
