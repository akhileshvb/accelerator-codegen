UPDATE region SET r_name='ASIA ' WHERE r_comment='q5 edge region';
UPDATE nation SET n_name=CASE n_name WHEN 'Q5 EDGE A' THEN 'Q5 EDGE A ' ELSE 'q5 edge a' END WHERE n_comment='q5 tied nation';
