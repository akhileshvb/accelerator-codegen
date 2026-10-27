-- SEC-EDGAR Q24 with its parameters as $n (year as $1, uom as $2); spec: eval/bench/sec_edgar/params/q24.json
SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total
FROM num n
LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh
WHERE n.uom = $2 AND n.ddate BETWEEN $1*10000+101 AND $1*10000+1231
  AND n.value IS NOT NULL
  AND p.adsh IS NULL
GROUP BY n.tag, n.version
HAVING COUNT(*) > 10
ORDER BY cnt DESC
LIMIT 100
