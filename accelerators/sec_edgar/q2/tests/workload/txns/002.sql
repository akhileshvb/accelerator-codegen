-- Amendment: a new filing restating an existing one with changed values; the old one is marked.
INSERT INTO sub SELECT '0009990004-24-000003', cik, name || ' (amended)', sic, countryba, stprba, cityba, countryinc, '10-K/A', period, fy, fp, 20240801, accepted, 0, nciks, afs, wksi, fye, instance
FROM sub WHERE adsh = (SELECT adsh FROM num WHERE uom = 'USD' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
INSERT INTO num SELECT '0009990004-24-000003', tag, version, ddate, qtrs, uom, coreg, value * 1.1, footnote
FROM num WHERE adsh = (SELECT adsh FROM num WHERE uom = 'USD' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1) AND rowid % 3 <> 0;
UPDATE sub SET prevrpt = 1 WHERE adsh = (SELECT adsh FROM num WHERE uom = 'USD' AND adsh <> '0009990004-24-000003' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
