UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'Q10 Coastal')
WHERE c_name = 'Q10-12';
