UPDATE nation SET n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'AMERICA' AND r_comment <> 'q8-region') WHERE n_comment = 'q8-nation';
DELETE FROM region WHERE r_comment = 'q8-region';
