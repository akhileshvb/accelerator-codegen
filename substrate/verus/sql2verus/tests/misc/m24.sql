SELECT a.country, b.country AS c2, COUNT(*) AS n FROM users a, users b WHERE a.score < b.score GROUP BY a.country, b.country ORDER BY a.country, c2
