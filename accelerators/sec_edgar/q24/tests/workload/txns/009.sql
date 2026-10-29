-- a big batch of filings at once: copies of every filing with anti-join rows, and the tie groups again
INSERT INTO sub SELECT * REPLACE ('bb-' || adsh AS adsh) FROM sub WHERE adsh IN (SELECT DISTINCT adsh FROM num WHERE uom IN ('USD', 'shares') AND ddate BETWEEN 20230101 AND 20241231) AND adsh NOT LIKE 'bb-%' AND adsh NOT LIKE 'cp-%' LIMIT 40;
INSERT INTO num SELECT * REPLACE ('bb-' || adsh AS adsh) FROM num WHERE ('bb-' || adsh) IN (SELECT adsh FROM sub);
INSERT INTO pre SELECT * REPLACE ('bb-' || adsh AS adsh) FROM pre WHERE ('bb-' || adsh) IN (SELECT adsh FROM sub);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-3', 'T' || lpad(g::VARCHAR, 3, '0'), 'v', 20310630, 4, 'TIE', 1.0 FROM range(0, 105, 3) a(g);
