SELECT u.user_id, COUNT(e.id) AS n FROM users u LEFT JOIN events e ON u.user_id = e.user_id GROUP BY u.user_id ORDER BY u.user_id
