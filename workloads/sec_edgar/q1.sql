-- SEC-EDGAR Q1 with its parameter as $1 (stmt as $1); spec: eval/bench/sec_edgar/params/q1.json
SELECT stmt, rfile, COUNT(*) AS cnt,
       COUNT(DISTINCT adsh) AS num_filings,
       AVG(line) AS avg_line_num
FROM pre
WHERE stmt = $1
GROUP BY stmt, rfile
ORDER BY cnt DESC
