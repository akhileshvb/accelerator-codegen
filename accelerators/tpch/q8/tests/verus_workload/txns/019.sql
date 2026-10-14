UPDATE nation SET n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'AMERICA') WHERE n_name = 'BRAZIL';
