SELECT user_id, AVG(amount * 1.5 + 2) AS a FROM events GROUP BY user_id
