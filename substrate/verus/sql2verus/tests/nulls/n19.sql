SELECT t.id, u.v FROM t LEFT JOIN u ON t.id = u.id WHERE u.v > 3 OR t.id < 2 ORDER BY t.id, u.v
