DELETE FROM orders WHERE o_comment IN ('q4:seed:13', 'q4:variant', 'q4:variant2');
DELETE FROM lineitem WHERE l_comment IN ('q4:seed:13', 'q4:restored-shared', 'q4:duplicate-line');
DELETE FROM lineitem WHERE l_comment LIKE 'q4:text:%' OR l_comment LIKE 'q4:extreme:%';
DELETE FROM orders WHERE o_comment LIKE 'q4:text:%' OR o_comment LIKE 'q4:extreme:%';
