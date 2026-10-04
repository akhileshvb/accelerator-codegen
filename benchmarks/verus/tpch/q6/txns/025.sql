INSERT INTO lineitem
SELECT source.* FROM lineitem AS source CROSS JOIN range(2) AS copies(copy_number)
WHERE l_comment = 'q6-boundary-7';
