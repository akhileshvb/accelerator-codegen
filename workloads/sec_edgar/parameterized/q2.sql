-- SEC-EDGAR Q2 with its parameters as $n (fy as $1, uom as $2); spec: eval/bench/sec_edgar/params/q2.json
SELECT s.name, n.tag, n.value
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN (
    SELECT adsh, tag, MAX(value) AS max_value
    FROM num
    WHERE uom = $2 AND value IS NOT NULL
    GROUP BY adsh, tag
) m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value
WHERE n.uom = $2 AND s.fy = $1 AND n.value IS NOT NULL
ORDER BY n.value DESC, s.name, n.tag
LIMIT 100
