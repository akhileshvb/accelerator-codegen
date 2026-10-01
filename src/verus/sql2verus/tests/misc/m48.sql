SELECT name FROM users WHERE user_id IN (SELECT user_id FROM events UNION SELECT user_id FROM users WHERE premium) ORDER BY name
