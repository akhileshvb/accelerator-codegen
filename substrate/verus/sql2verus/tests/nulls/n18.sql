SELECT u.id, t.s FROM u LEFT JOIN t ON u.a = t.a AND t.b > 1 ORDER BY u.id, t.s
