SELECT x.k, COUNT(*) AS c FROM (SELECT DISTINCT kind AS k, user_id FROM events) x GROUP BY x.k ORDER BY x.k
