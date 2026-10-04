UPDATE nation SET n_regionkey=(SELECT r_regionkey FROM region WHERE r_name='EUROPE'), n_comment='q5 moved nation'
WHERE n_nationkey=(SELECT min(n_nationkey) FROM nation JOIN region ON n_regionkey=r_regionkey WHERE r_name='ASIA');
