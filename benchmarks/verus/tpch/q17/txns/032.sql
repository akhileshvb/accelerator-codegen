DELETE FROM lineitem WHERE l_comment LIKE 'q17-base-%' OR l_comment = 'q17-boundary';
DELETE FROM part WHERE p_name LIKE 'q17-fixture-%' OR p_name IN ('q17-boundary', 'q17-text-variant');
