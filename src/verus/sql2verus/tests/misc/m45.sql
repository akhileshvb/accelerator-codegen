SELECT u.name, COUNT(e.id) AS n FROM events e RIGHT JOIN users u ON u.user_id = e.user_id GROUP BY u.name ORDER BY u.name
