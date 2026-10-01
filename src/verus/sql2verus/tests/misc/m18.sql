SELECT user_id FROM users WHERE signup >= $1 AND country = $2 AND NOT EXISTS (SELECT * FROM events WHERE events.user_id = users.user_id) ORDER BY user_id
