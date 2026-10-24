UPDATE customer SET c_nationkey=CAST(split_part(c_name, ':', 2) AS INTEGER), c_comment='q5 fixture customer' WHERE c_comment='q5 relocating customer';
