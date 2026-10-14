SELECT u.name, (SELECT COUNT(*) FROM events e WHERE e.user_id = u.user_id) AS n FROM users u ORDER BY u.name, n
