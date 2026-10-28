SELECT t.id, SUM(u.v) AS sv, COUNT(u.v) AS cv FROM t LEFT JOIN u ON t.id = u.id GROUP BY t.id ORDER BY t.id
