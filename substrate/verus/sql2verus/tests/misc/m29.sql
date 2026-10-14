SELECT u.user_id FROM users u FULL OUTER JOIN events e ON u.user_id = e.user_id
