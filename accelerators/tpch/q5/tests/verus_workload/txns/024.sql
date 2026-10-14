UPDATE nation SET n_regionkey=(SELECT r_regionkey FROM region WHERE r_name='ASIA'), n_comment='q5 restored nation' WHERE n_comment='q5 moved nation';
