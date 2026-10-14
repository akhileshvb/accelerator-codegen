SELECT t.kind, COUNT(*) AS c FROM (SELECT kind, SUM(amount) AS s FROM events GROUP BY kind) t GROUP BY t.kind
