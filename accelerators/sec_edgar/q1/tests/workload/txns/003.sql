-- Amendment: a new filing (its own adsh) restating an existing one, which it marks (prevrpt = 1).
INSERT INTO sub SELECT '0009990004-24-000003', cik, name, sic, countryba, stprba, cityba, countryinc, '10-K/A', period, fy, fp, 20240801, accepted, 0, nciks, afs, wksi, fye, instance
FROM sub WHERE adsh = (SELECT adsh FROM pre GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
INSERT INTO pre SELECT '0009990004-24-000003', report, line, stmt, inpth, rfile, tag, version, plabel, negating
FROM pre WHERE adsh = (SELECT adsh FROM pre GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1) AND line % 7 <> 0;
UPDATE sub SET prevrpt = 1 WHERE adsh = (SELECT adsh FROM pre WHERE adsh <> '0009990004-24-000003' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
