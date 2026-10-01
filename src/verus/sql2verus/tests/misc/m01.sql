SELECT kind, SUM(amount) AS total, COUNT(*) AS n FROM events WHERE ok GROUP BY kind ORDER BY kind
