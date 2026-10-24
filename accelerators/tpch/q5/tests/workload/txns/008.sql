UPDATE supplier SET s_nationkey=(SELECT c_nationkey FROM customer WHERE c_comment='q5 relocating customer'), s_comment='q5 relocating supplier'
WHERE s_name='Q5 supplier:' || (SELECT split_part(c_name, ':', 2) FROM customer WHERE c_comment='q5 relocating customer') AND s_comment='q5 fixture supplier';
