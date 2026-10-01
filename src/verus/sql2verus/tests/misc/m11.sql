SELECT u.name, SUM(e.amount) AS s FROM users u JOIN events e ON u.user_id = e.user_id GROUP BY u.name
