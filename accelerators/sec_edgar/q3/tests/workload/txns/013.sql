-- A real company files again under another name in the same year.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT 'RN-1', cik, name || ' (renamed)', sic, fy, 0 FROM sub WHERE fy = 2022 ORDER BY adsh LIMIT 1;
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT 'RN-1', 'T', 'v', 20220101, 4, 'USD', 123456789.0;
