-- 150 companies tied above the average and 150 below: the cut at 100 falls inside the tie.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT 'TIEQ-' || i, 8200000 + i, 'Tie ' || i, 1000, 2096, 0 FROM range(300) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT 'TIEQ-' || i, 'T', 'v', 20960101, 4, 'TQ', CASE WHEN i < 150 THEN 1000.0 ELSE 1.0 END FROM range(300) t(i);
