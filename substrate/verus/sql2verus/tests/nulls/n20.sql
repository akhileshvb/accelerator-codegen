SELECT t.id, MAX(u.v) AS mv, MIN(u.v) AS lv FROM t LEFT JOIN u ON t.id = u.id GROUP BY t.id ORDER BY t.id
