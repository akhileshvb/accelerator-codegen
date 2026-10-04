UPDATE part SET p_type = CASE p_type WHEN 'LARGE STEEL' THEN 'PROMO STEEL' ELSE 'LARGE STEEL' END WHERE p_comment IN ('q8-part-a', 'q8-part-b');
