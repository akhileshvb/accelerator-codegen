SELECT kind, COUNT(*) AS c FROM events GROUP BY kind HAVING COUNT(*) > $1 ORDER BY c DESC, kind
