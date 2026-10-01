SELECT e.kind, u.name FROM events e RIGHT JOIN users u ON u.user_id = e.user_id
