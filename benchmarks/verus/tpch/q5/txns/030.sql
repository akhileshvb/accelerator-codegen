UPDATE nation SET n_regionkey=(SELECT r_regionkey FROM region WHERE r_name='ASIA') WHERE n_regionkey=(SELECT r_regionkey FROM region WHERE r_comment='q5 pacific region');
DELETE FROM region WHERE r_comment='q5 pacific region';
