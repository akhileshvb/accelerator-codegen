SELECT DATE_TRUNC('month', d) AS m, DATE_TRUNC('week', ts) AS w, DATE_TRUNC('year', d) AS y, DATE_TRUNC('hour', ts) AS h, DATE_TRUNC('day', d) AS dd, DATE_TRUNC('second', ts) AS s FROM ev ORDER BY id
