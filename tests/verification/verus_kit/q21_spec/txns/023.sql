UPDATE nation SET n_name = CASE WHEN n_name = 'PERU' THEN 'PERU ' ELSE n_name END WHERE n_nationkey = 23;
