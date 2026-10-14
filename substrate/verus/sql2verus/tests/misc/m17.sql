SELECT name FROM users u WHERE u.user_id IN (SELECT e.user_id FROM events e WHERE e.amount > 100.0 AND e.kind = u.name) ORDER BY name
