SELECT user_id, COUNT(*) AS n FROM events WHERE ts > (SELECT MAX(ts) FROM events) - INTERVAL '7' DAY GROUP BY user_id ORDER BY user_id
