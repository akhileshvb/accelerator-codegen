SELECT user_id, ROW_NUMBER() OVER (ORDER BY user_id) AS r FROM users
