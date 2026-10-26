-- SEC-EDGAR Q6 with its parameters as $n (fy as $1, stmt as $2, uom as $3); spec: eval/bench/sec_edgar/params/q6.json
SELECT s.name, p.stmt, n.tag, p.plabel,
       SUM(n.value) AS total_value, COUNT(*) AS cnt
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
WHERE n.uom = $3 AND p.stmt = $2 AND s.fy = $1
  AND n.value IS NOT NULL
GROUP BY s.name, p.stmt, n.tag, p.plabel
ORDER BY total_value DESC, s.name, n.tag, p.plabel
LIMIT 200
