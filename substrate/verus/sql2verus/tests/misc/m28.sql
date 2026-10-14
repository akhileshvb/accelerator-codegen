SELECT kind, SUM(amount) AS s FROM events GROUP BY kind ORDER BY kind LIMIT 5 OFFSET 2
