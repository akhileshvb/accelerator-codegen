SELECT country, COUNT(DISTINCT user_id) AS u FROM users GROUP BY country ORDER BY u DESC, country
