UPDATE supplier SET s_nationkey=CAST(split_part(s_name, ':', 2) AS INTEGER), s_comment='q5 fixture supplier' WHERE s_comment='q5 relocating supplier';
