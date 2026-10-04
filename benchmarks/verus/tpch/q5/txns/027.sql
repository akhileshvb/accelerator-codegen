UPDATE supplier SET s_nationkey=CAST(split_part(s_name, ':', 2) AS INTEGER), s_comment='q5 fixture supplier' WHERE s_comment='q5 territory supplier';
UPDATE customer SET c_nationkey=CAST(split_part(c_name, ':', 2) AS INTEGER), c_comment='q5 fixture customer' WHERE c_comment='q5 territory customer';
DELETE FROM nation WHERE n_comment='q5 new territory';
