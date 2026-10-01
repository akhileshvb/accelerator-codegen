SELECT kind, MIN(amount) AS lo, MAX(amount) AS hi FROM events GROUP BY kind ORDER BY kind
