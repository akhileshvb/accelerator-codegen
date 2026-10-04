INSERT INTO region SELECT seed.* REPLACE (((SELECT max(r_regionkey) FROM region)+1) AS r_regionkey, 'PACIFIC' AS r_name, 'q5 pacific region' AS r_comment) FROM region seed ORDER BY r_regionkey LIMIT 1;
UPDATE nation SET n_regionkey=(SELECT r_regionkey FROM region WHERE r_comment='q5 pacific region') WHERE n_regionkey=(SELECT r_regionkey FROM region WHERE r_name='ASIA');
