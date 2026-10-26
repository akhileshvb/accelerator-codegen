SELECT t.id, u.name FROM t LEFT JOIN u ON t.a = u.a WHERE u.name IS NULL ORDER BY t.id
